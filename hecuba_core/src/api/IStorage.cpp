#include "IStorage.h"
#include "UUID.h"

#include <regex>
#include <boost/uuid/uuid.hpp>
#include "debug.h"

#define ISKEY true

/***
    d = //dictionary [int, float]:[values]
    for (keysIterator s = d.begin(); s != d.end(); s ++) {
        (*s)        <== buffer con int+float (*m_ptr)
        (s)         <== Iterador
        (s->xxx)    <== NOT SUPPORTED
    }
***/
IStorage::keysIterator IStorage::begin() {
        // Create thread and ask Casandra for data

        config_map iterator_config = currentSession->config;
        iterator_config["type"]="keys"; // Request a prefetcher for 'keys' only
        return keysIterator(this, currentSession->getStorageInterface()->get_iterator(
                    dataAccess->get_metadata()
                    , iterator_config));
}

IStorage::keysIterator IStorage::end()   { return keysIterator(); } // NULL is the placeholder for last element

IStorage::IStorage(HecubaSession* session, std::string id_model, std::string id_object, uint64_t* storage_id, CacheTable* dataAccess) {
	this->currentSession = session;
	this->id_model = id_model;
	this->id_obj = id_object;

	this->storageid = storage_id;
	this->dataAccess = dataAccess;
	this->dataWriter = dataAccess->get_writer();

    this->data = NULL;
}

IStorage::~IStorage() {
	delete(dataAccess);
}


std::string IStorage::generate_numpy_table_name(std::string attributename) {
    /* ksp.DUUIDtableAttribute extracted from hdict::make_val_persistent */
    //std::cout << "DEBUG: IStorage::generate_numpy_table_name: BEGIN attribute:"<<attributename<<std::endl;
    std::regex what("-");
    std::string name;
    // Obtain keyspace and table_name from id_obj (keyspace.table_name)
    uint32_t pos = id_obj.find_first_of(".");
    //std::string ksp = id_obj.substr(0, pos);
    std::string table_name = id_obj.substr(pos+1, id_obj.length()); //skip the '.'

    // Generate a new UUID for this attribute
    uint64_t *c_uuid = UUID::generateUUID(); // UUID for the new object
    std::string uuid = std::regex_replace(UUID::UUID2str(c_uuid), what, "_");

    // attributename contains "name1.name2.....attributename" therefore keep the last attribute name TODO: Fix this 'name.name...' nightmare
    //name = ksp + ".D" + uuid + table_name + attributename.substr(attributename.find_last_of('_'), attributename.size());
    name = "D" + uuid + table_name + attributename;

    name = name.substr(0,48);
    //std::cout << "DEBUG: IStorage::generate_numpy_table_name: END "<<name<<std::endl;
    return name;
}

uint64_t* IStorage::getStorageID() {
    return storageid;
}

const std::string& IStorage::getName() const {
    return id_obj;
}

void
IStorage::sync(void) {
    this->dataWriter->flush_elements();
}


/* Transform IStorage pointers to StorageIDs pointers
    Args:
        dst: block of memory to update
        value_type: string from ObjSpec specifying the type to transform,
            anything different from a basic type will be transformed
        src: pointer to a block of memory with a value (if basic type), a
            pointer to char (string) or a pointer to an IStorage object
        src_size: size of src


     src ----> +---------+
               |     *---+------------------->+----------+
               +---------+                    | IStorage-+-------->+-----------+
                                              +----------+         | StorageID |
    Generates:                                                     +-----------+

     dst ----> +---------+
               |     *---+------------------->+-----------+ (This memory is allocated and contains a copy)
               +---------+                    | StorageID'|
                                              +-----------+
    But:
     src ----> +---------+
               | 42      |
               +---------+
    Generates:

     dst ----> +---------+
               | 42      |
               +---------+

*/
bool IStorage::convert_IStorage_to_UUID(char * dst, const std::string& value_type, const void* src, int64_t src_size) const {
    bool isIStorage = false;
    void * result;
    DBG( "convert_IStorage_to_UUID " + value_type );
    if (!ObjSpec::isBasicType(value_type)) {
        DBG( "convert_IStorage_to_UUID NOT BASIC" );
        result = (*(IStorage **)src)->getStorageID(); // 'src' MUST be a valid pointer or it will segfault here...
#if 0
        // Minimal Check for UUID
        boost::uuids::uuid u;
        memcpy(&u, result, 16);
        boost::uuids::uuid::variant_type variant = u.variant();
        boost::uuids::uuid::version_type version = u.version();
        if ( ! ((variant == boost::uuids::uuid::variant_rfc_4122) && (version == boost::uuids::uuid::version_name_based_sha1))) {
            throw ModuleException("IStorage:: Set Item. Wrong UUID format for object... is it a pointer to an IStorage?");
        }
#endif
        // It seems like a valid UUID
        void * sid = malloc(sizeof(uint64_t)*2);
        memcpy(sid, result, sizeof(uint64_t)*2);
        memcpy(dst, &sid, src_size) ;
        isIStorage = true;
    } else{ // it is a basic type, just copy the value
        //if value_type is a string, copy it to a new variable to be independent of potential memory free from the user code
        DBG( "convert_IStorage_to_UUID BASIC is " + value_type );
        if (!value_type.compare(std::string{"text"})) {
            result = *(char**)src; // 'src' MUST be a valid pointer or it will segfault here...
            char* tmp = (char*)malloc (strlen((char*)result)+1);
            memcpy(tmp, (char*)result, strlen((char*)result)+1); // Copy the string content
            memcpy(dst, &tmp, src_size); // Copy the address
        } else {
            memcpy(dst, src, src_size); // Copy the content
        }
    }
    return isIStorage;
}

/* deep_copy_attribute_buffer: Creates a copy of a block of memory containing values to store in a table, complex types are also copied.
 *  iskey   : The buffer corresponds to a key? or is it a column?
 *  src     : pointer to source memory block
 *  src_size: length of the source memory block
 *  num_attrs: Number of attributes inside the 'src' memory block
 * return a NEW block of memory with the same content as 'src' but creating NEW copies for internal complex data (currently STRINGS).
 */
void * IStorage::deep_copy_attribute_buffer(bool isKey, const void* src, uint64_t src_size, uint32_t num_attrs) const {

    /** WARNING: The 'src' buffer comes from user, therefore the fields order is
     * specified by the ObjSpec which may or may not (possibly the latter)
     * coincide with the format needed to access the database.
     * This method reorders the resulting buffer to be suitable for this
     * access.*/

    void * dst = malloc(src_size);

    // Process src to generate memory to complex types: UUIDs, strings,...
    DataModel* model = this->currentSession->getDataModel();
    ObjSpec ospec = model->getObjSpec(this->id_model);

    const TableMetadata* writerMD = dataWriter->get_metadata();

    DBG( "deep_copy_attribute_buffer num attributes="<<num_attrs);
    int64_t value_size;
    uint64_t offset=0;

    // Traverse the buffer following the user order...
    for (uint32_t i=0; i < num_attrs; i++) {

        DBG("  deep_copy_attribute_buffer offset ="<<offset);

        std::string column_name;
        std::string value_type;
        const ColumnMeta *c;
        // Only 2 cases supported: keys or values
        if (isKey) {
            column_name = ospec.getIDObjFromKey(i);
            value_type = ospec.getIDModelFromKey(i);
            c = writerMD->get_single_key(column_name);
        } else {
            column_name = ospec.getIDObjFromCol(i);
            value_type = ospec.getIDModelFromCol(i);
            c = writerMD->get_single_column(column_name);
        }
        value_size = c->size;

        // Convert each attribute and reorder it to the right position...
        convert_IStorage_to_UUID(((char *)dst)+c->position, value_type, ((char*)src) + offset, value_size);

        offset += value_size;
    }

    return dst;
}

void
IStorage::send_values(const void* value) {
    DBG("START");
    const TableMetadata* writerMD = dataWriter->get_metadata();
    DataModel* model = this->currentSession->getDataModel();

    ObjSpec ospec = model->getObjSpec(this->id_model);

    std::shared_ptr<const std::vector<ColumnMeta> > columns = writerMD->get_values();
    uint32_t numcolumns = columns->size();

    uint64_t offset = 0;
    const char* src = (char*)value;
    // Traverse the buffer following the user order...
    for (uint32_t i=0; i < numcolumns; i++) {
        std::string column_name = ospec.getIDObjFromCol(i);
        std::string value_type = ospec.getIDModelFromCol(i);
        const ColumnMeta *c = writerMD->get_single_column(column_name);
        int64_t value_size= c->size;
        DBG(" -->  traversing column '"<<column_name<< "' of type '" << value_type<<"'" );
        if (!ObjSpec::isBasicType(value_type)) {
            if (value_type.compare("hecuba.hnumpy.StorageNumpy") == 0) {
                IStorage * result = *(IStorage **)(src+offset); // 'src' MUST be a valid pointer or it will segfault here...
                if (!result->isStream()) { // If the object did not have Stream enabled, enable it now as we are going to stream it...
                    result->enableStream(UUID::UUID2str(result->getStorageID()));
                }
                result->send();
                DBG("   -->  sent "<< UUID::UUID2str(result->getStorageID()));
            }
        }
        offset += value_size;
    }
    DBG("END");
}

/* Args:
    key and value are pointers to a block of memory with the values (if basic types) or pointers to IStorage or strings:
    key/value -.
               |
               V
               +---------+
               |     *---+------------------->+----------+
               +---------+                    | IStorage |-------->+-----------+
               | 42      |                    +----------+         | StorageID |
               +---------+                                         +-----------+
               | 0.66    |
               +---------+
               |     *---+------------------->+--------+
               +---------+                    | hola\0 |
                                              +--------+

    Create a copy of it, normalizing the pointer to other structs keeping just the storageID:
    cc_key/cc_val
               |
               V
               +---------+
               |     *---+------------------->+-----------+ (newly allocated and copied)
               +---------+                    | StorageID'|
               | 42      |                    +-----------+
               +---------+
               | 0.66    |
               +---------+
               |     *---+------------------->+--------+ (newly allocated and copied)
               +---------+                    | hola\0'|
                                              +--------+

    Therefore, 'key' and 'value' may be freed after this method.
*/
void
IStorage::writeTable(const void* key, const void* value, const enum IStorage::valid_writes mytype) {
	/* PRE: key and value arrives already coded as expected */

    void * cc_val;

    const TableMetadata* writerMD = dataWriter->get_metadata();


    DBG( "IStorage::writeTable enter" );

    DataModel* model = this->currentSession->getDataModel();

    ObjSpec ospec = model->getObjSpec(this->id_model);
    //std::cout << "DEBUG: IStorage::setItem: obtained model for "<<id_model<<std::endl;

    std::string value_type;
    if (ospec.getType() == ObjSpec::valid_types::STORAGEDICT_TYPE) {
        if (mytype != SETITEM_TYPE) {
            throw ModuleException("IStorage:: Set Item on a non Dictionary is not supported");
        }
        // Dictionary values may have N  columns, create a new structure with all of them normalized.
        DBG( "IStorage::WriteTable malloc("<<writerMD->get_values_size()<<")");
        std::shared_ptr<const std::vector<ColumnMeta> > columns = writerMD->get_values();
        uint32_t numcolumns = columns->size();
        cc_val = deep_copy_attribute_buffer(!ISKEY, value, writerMD->get_values_size(), numcolumns);

    } else if (ospec.getType() == ObjSpec::valid_types::STORAGEOBJ_TYPE) {
        if (mytype != SETATTR_TYPE) {
            throw ModuleException("IStorage::writeTable Set Attr on a non Object is not supported");
        }
        int64_t value_size = writerMD->get_single_column((char*)key)->size;
        cc_val = malloc(value_size); // This memory will be freed after the execution of the query (at callback)

        std::string value_type = ospec.getIDModelFromColName(std::string((char*)key));
        convert_IStorage_to_UUID((char *)cc_val, value_type, value, value_size);
    } else {
        throw ModuleException("IStorage::writeTable Set individual components of a StorageNumpy is not supported");
    }

    //std::cout << "DEBUG: IStorage::setItem: After creating value object "<<std::endl;

    // STORE THE ENTRY IN TABLE (Ex: keys + value ==> storage_id del numpy)

    std::pair<uint16_t, uint16_t> keySize = writerMD->get_keys_size();
    uint64_t partKeySize = keySize.first;
    uint64_t clustKeySize = keySize.second;
    DBG("IStorage::writeTable --> partKeySize = "<<partKeySize<<" clustKeySize = "<< clustKeySize);

    void *cc_key= NULL;
    if (mytype == SETITEM_TYPE) {

        std::shared_ptr<const std::vector<ColumnMeta> > columns = writerMD->get_keys();
        uint32_t numcolumns = columns->size();
        cc_key = deep_copy_attribute_buffer(ISKEY, key, partKeySize+clustKeySize, numcolumns);

    } else {
        uint64_t* sid = this->getStorageID();
        void* c_key = malloc(2*sizeof(uint64_t)); //uuid
        std::memcpy(c_key, sid, 2*sizeof(uint64_t));

        cc_key = malloc(sizeof(uint64_t *)); // This memory will be freed after the execution of the query (at callback)
        std::memcpy(cc_key, &c_key, sizeof(uint64_t *));
    }

    if (mytype == SETITEM_TYPE) {
        //TODO currently our c++ API only supports instantiation of persistent objects. If we add support to volatile objects
        // we should extend this funtion to persist a volatile object assigned to a persistent object
        const TupleRow* trow_key = this->dataAccess->get_new_keys_tuplerow(cc_key);
        const TupleRow* trow_values = this->dataAccess->get_new_values_tuplerow(cc_val);
        if (this->isStream()) {
            this->dataWriter->send_event(trow_key, trow_values); // stream value (storage_id/value)
            send_values(value); // If value is an IStorage type stream its contents also
        }
        this->dataAccess->put_crow(trow_key, trow_values);
        delete(trow_key);
        delete(trow_values);

    } else { // SETATTR
        char* attr_name = (char*) key;
        #if 0
        /* TODO: Enable this code when implementing storageobj streaming */
        if (this->isStream() {
            this->dataWriter->send_event(cc_key, cc_val, attr_name); // stream a single attribute
        }
        #endif
        this->dataWriter->write_to_cassandra(cc_key, cc_val, attr_name);
        // TODO: add here a call to send for attribute (NOT SUPPORTED YET)
    }
}

void IStorage::setAttr(const char *attr_name, void* value) {
    /* PRE: value arrives already coded as expected: block of memory with pointers to IStorages or basic values*/
    //std::cout << "DEBUG: IStorage::setAttr: "<<std::endl;
    writeTable(attr_name, value, SETATTR_TYPE);
}

void IStorage::setAttr(const char *attr_name, IStorage* value) {
    /* 'writetable' expects a block of memory with pointers to IStorages, therefore add an indirection */
    writeTable(attr_name, (void *) &value, SETATTR_TYPE);
}

void IStorage::setItem(void* key, void* value) {
    /* PRE: value arrives already coded as expected: block of memory with pointers to IStorages or basic values*/
    writeTable(key, value, SETITEM_TYPE);
}

void IStorage::setItem(void* key, IStorage * value){
    /* 'writetable' expects a block of memory with pointers to IStorages, therefore add an indirection */
    writeTable(key, (void *) &value, SETITEM_TYPE);
}

void IStorage::send(void) {
    DataModel* model = this->currentSession->getDataModel();
    ObjSpec ospec = model->getObjSpec(this->id_model);
    DBG("DEBUG: IStorage::send: obtained model for "<<this->id_model );
    if (ospec.getType() == ObjSpec::valid_types::STORAGENUMPY_TYPE) {
         DBG("DEBUG: IStorage::send: sending numpy. Size "<< numpy_metas.get_array_size());
         dataWriter->send_event((char *) data, numpy_metas.get_array_size());
    } else {
            throw ModuleException("IStorage:: Send only whole StorageNumpy implemented.");
#if 0
        if (ospec.getType() == ObjSpec::valid_types::STORAGEOBJ_TYPE) {
            // Traverse all attributes and send everything
            for (auto i: colsnames) {
                this->dataAccess->put_crow(cc_key, cc_val);
            }
        } else {
            throw ModuleException("IStorage:: Send only whole StorageNumpy implemented.");
        }
#endif
    }
}

#if 0
void IStorage::send(void* key, void* value) {
    DataModel* model = this->currentSession->getDataModel();

    ObjSpec ospec = model->getObjSpec(this->id_model);
    DBG("DEBUG: IStorage::send: obtained model for "<<id_model << " obj stream?"<<ospec.isStream());

    if (this->isStream()) {
        // dictionary case
        uint64_t offset=0;
        const TableMetadata* writerMD = dataWriter->get_metadata();
        uint32_t numcolumns = writerMD->get_values()->size();
        DBG( "Stream send numcols="<<numcolumns);
        void * cc_val = malloc(writerMD->get_values_size()); // This memory will be freed after the execution of the query (at callback)

        for (uint32_t i=0; i < numcolumns; i++) {
            int64_t value_size;

            DBG("Send offset ="<<offset);
            value_size = writerMD->get_values_size(i);
            std::string value_type = ospec.getIDModelFromCol(i);

            bool isIStorage = convert_IStorage_to_UUID(((char *)cc_val)+offset, value_type, ((char*)value) + offset, value_size);
            if (isIStorage) {
                IStorage *myobj = *(IStorage **)((char *)value + offset);
                if (!myobj->isStream()) {
                    std::string topic = std::string(currentSession->UUID2str(myobj->getStorageID()));
                    DBG("  Object "<< topic <<" is not stream enabled. Enabling.");
                    myobj->enableStream(topic);
                }
                myobj->send();
            }

            offset += value_size;
        }
        // storageobj case: key is the attribute name TODO

        this->dataWriter->send_event(key, cc_val); // stream AND store value in Cassandra

    } else {
        throw ModuleException("IStorage:: Send on an object that has no stream capability");
    }
}

void IStorage::send(void* key, IStorage* value) {
    send(key, (void *) &value);
}
#endif

void IStorage::extractFromQueryResult(std::string value_type, uint32_t value_size, void *query_result, void *valuetoreturn) const{
    if (!ObjSpec::isBasicType(value_type)) {
        IStorage *read_object = this->currentSession->createObject(value_type.c_str(), *(uint64_t **)query_result);
        memcpy(valuetoreturn, &read_object, sizeof(IStorage *));
    } else {
        if (value_type == "text") {
            char *str = *(char**)query_result;
            value_size = strlen(str) + 1;
            char *tmp = (char *) malloc(value_size);
            memcpy(tmp, str, value_size);
            memcpy(valuetoreturn, &tmp, sizeof(char*));
        } else {
            memcpy(valuetoreturn, query_result, value_size);
        }
    }
}

/* Given a result from a cassandra query, extract all elements into valuetoreturn buffer*/
/* type = KEYS/COLUMNS TODO: add ALL to support the iteration for both keys and values (pythom items method) */

void IStorage::extractMultiValuesFromQueryResult(void *query_result, void *valuetoreturn, int type) const {
    uint32_t attr_size;
    DataModel* model = this->currentSession->getDataModel();
    ObjSpec ospec = model->getObjSpec(this->id_model);

    const TableMetadata* writerMD = dataWriter->get_metadata();

    uint64_t offset = 0; // offset in user buffer
    std::shared_ptr<const std::vector<ColumnMeta> > metas;
    if (type == COLUMNS) {
        metas = writerMD->get_values();
        attr_size = writerMD->get_values_size();
    } else {
        metas = writerMD->get_keys();
        std::pair<uint16_t,uint16_t> keys_size = writerMD->get_keys_size();
        attr_size=keys_size.first + keys_size.second;
    }

    char *valuetmp = (char*) malloc(attr_size);

    std::string attr_name;
    std::string attr_type;
    const ColumnMeta *c;
    for (uint64_t pos = 0; pos<metas->size(); pos++) {
        if (type == COLUMNS) {
            attr_name = ospec.getIDObjFromCol(pos);
            attr_type = ospec.getIDModelFromCol(pos);
            c = writerMD->get_single_column(attr_name);
        } else {
            attr_name = ospec.getIDObjFromKey(pos);
            attr_type = ospec.getIDModelFromKey(pos);
            c = writerMD->get_single_key(attr_name);
        }
        attr_size = c->size;

        // c->position contains offset in query_result.
        extractFromQueryResult(attr_type, attr_size, ((char*)query_result) + c->position, valuetmp+offset);

        offset += attr_size;
    }

    // Copy Result to user:
    //   If a single basic type value is returned then the user passes address
    //   to store the value, otherwise we allocate the memory to store all the
    //   values.
    if (metas->size() == 1) {
        memcpy(valuetoreturn, valuetmp, attr_size);
    } else {
        memcpy(valuetoreturn, &valuetmp, attr_size);
    }
}

/* Return:
 *  memory reference to datatype (must be freed by user) */
void IStorage::getAttr(const char* attr_name, void* valuetoreturn) const{

    char *keytosend = (char*) malloc(sizeof(char*));
    char *uuidmem = (char*) malloc(sizeof(uint64_t)*2);
    int value_size = dataAccess->get_metadata()->get_values_size(dataAccess->get_metadata()->get_columnname_position(attr_name));

    memcpy(keytosend, &uuidmem, sizeof(char*));
    memcpy(uuidmem, storageid, sizeof(uint64_t)*2);

    std::vector<const TupleRow *> result = dataAccess->retrieve_from_cassandra(keytosend, attr_name);

    if (result.empty()) throw ModuleException("IStorage::getAttr: attribute " + std::string(attr_name) + " not found in object " + id_obj );
    char *query_result= (char*)result[0]->get_payload();

    DataModel* model = this->currentSession->getDataModel();
    ObjSpec ospec = model->getObjSpec(this->id_model);
    std::string value_type = ospec.getIDModelFromColName(attr_name);

    extractFromQueryResult(value_type, value_size, query_result, valuetoreturn);

    // Free the TupleRows...
    for(auto i:result) {
        delete(i);
    }


    return;
}

void IStorage::getItem(const void* key, void *valuetoreturn) const{
    const TableMetadata* writerMD = dataAccess->get_metadata();
    /* PRE: value arrives already coded as expected: block of memory with pointers to IStorages or basic values*/
    std::pair<uint16_t, uint16_t> keySize = writerMD->get_keys_size();
    int key_size = keySize.first + keySize.second;

    std::shared_ptr<const std::vector<ColumnMeta> > columns = writerMD->get_keys();

    void *keytosend = deep_copy_attribute_buffer(ISKEY, key, key_size, columns->size());

    std::vector<const TupleRow *> result = dataAccess->get_crow(keytosend);

    if (result.empty()) throw ModuleException("IStorage::getItem: key not found in object "+ id_obj);

    char *query_result= (char*)result[0]->get_payload();

    // WARNING: The order of fields in the TableMetadata and in the model may
    // NOT be the same! Traverse the TableMetadata and construct the User
    // buffer with the same order as the ospec. FIXME

    extractMultiValuesFromQueryResult(query_result, valuetoreturn, COLUMNS);

    // TODO this works only for dictionaries of one element. We should traverse the whole vector of values
    // TODO delete the vector of tuple rows and the tuple rows
    return;
}

void * IStorage::getNumpyData() const {
    return data;
}


void IStorage::setNumpyAttributes(ArrayDataStore* array_store, ArrayMetadata &metas, void* value) {
    this->arrayStore = array_store;
    this->numpy_metas = metas;
    DBG("DEBUG: IStorage::setNumpyAttributes: numpy Size "<< numpy_metas.get_array_size());

    //this->data = value;
    this->data = malloc(numpy_metas.get_array_size());
    if (value) {
        memcpy(data, value, numpy_metas.get_array_size());
    } else {
        std::list<std::vector<uint32_t>> coord = {};
        arrayStore->read_numpy_from_cas_by_coords(getStorageID(), metas, coord, data);
    }
}
bool IStorage::isStream() {
    return streamEnabled;
}

void IStorage::enableStream(std::string topic) {
    streamEnabled=true;
    this->dataWriter->enable_stream(topic.c_str(),(std::map<std::string, std::string>&)this->currentSession->config);
}
