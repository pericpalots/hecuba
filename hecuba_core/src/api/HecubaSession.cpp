#include "HecubaSession.h"
#include "ArrayDataStore.h"
#include "SpaceFillingCurve.h"
#include "IStorage.h"
#include "yaml-cpp/yaml.h"
#include "ObjSpec.h"
#include "DataModel.h"
#include "UUID.h"

#include "debug.h"

#include <cstdlib>
#include <vector>
#include <string>
#include <sstream>
#include <bits/stdc++.h>

#include <iostream>

 #include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

//#include "numpy/arrayobject.h" // FIXME to use the numpy constants NPY_*
#define NPY_ARRAY_C_CONTIGUOUS    0x0001
#define NPY_ARRAY_F_CONTIGUOUS    0x0002
#define NPY_ARRAY_OWNDATA         0x0004
#define NPY_ARRAY_FORCECAST       0x0010
#define NPY_ARRAY_ENSURECOPY      0x0020
#define NPY_ARRAY_ENSUREARRAY     0x0040
#define NPY_ARRAY_ELEMENTSTRIDES  0x0080
#define NPY_ARRAY_ALIGNED         0x0100
#define NPY_ARRAY_NOTSWAPPED      0x0200
#define NPY_ARRAY_WRITEABLE       0x0400
#define NPY_ARRAY_UPDATEIFCOPY    0x1000

// This code adds the decoding of YAML files to ObjSpec (NOT the other way around)
namespace YAML {
    template<>
        struct convert<DataModel::datamodel_spec> {
            static Node encode(const DataModel::datamodel_spec& obj) {
                Node node;

                return node;
            }
            static std::string mapUserType(std::string name) {
                /* Translates 'user type' to 'data model type'. numpy.ndarray --> hecuba.hnumpy.StorageNumpy */
                if ((name == "numpy.ndarray") || (name == "StorageNumpy")) {
                    return "hecuba.hnumpy.StorageNumpy";
                }
                name = ObjSpec::yaml_to_cass(name);
                return name;
            }
            static bool decode(const Node& node, DataModel::datamodel_spec& dmodel) {
                ObjSpec::valid_types obj_type;
                std::vector<std::pair<std::string, std::string>> partitionKeys;
                std::vector<std::pair<std::string, std::string>> clusteringKeys;
                std::vector<std::pair<std::string, std::string>> cols;
                std::string pythonString="";

                std::string attrName, attrType;
                if(!node.IsMap()) { return false; }

                const Node typespec = node["TypeSpec"];
                if (!typespec.IsSequence() || (typespec.size() != 2)) { return false; }
                bool streamEnabled = false;
                dmodel.id=typespec[0].as<std::string>();
                pythonString="class " + dmodel.id;

                if ((typespec[1].as<std::string>() == "StorageDict")){
                    pythonString="from hecuba import StorageDict\n\n" + pythonString;
                    pythonString += " (StorageDict):\n";
                    obj_type=ObjSpec::valid_types::STORAGEDICT_TYPE;

                    if (node["KeySpec"]) {
                        const Node keyspec =  node["KeySpec"];
                        // TODO: Check that keyspec is not null
                        if (!keyspec.IsSequence() || (keyspec.size() == 0)) { return false; }

                        pythonString+="   '''\n   @TypeSpec dict <<";


                        if (!keyspec[0].IsSequence() || (keyspec[0].size() != 2)) { return false; }

                        attrName=keyspec[0][0].as<std::string>();
                        attrType=keyspec[0][1].as<std::string>();
                        pythonString+=attrName+":"+attrType;
                        partitionKeys.push_back(std::pair<std::string,std::string>(attrName,mapUserType(attrType)));

                        for (uint32_t i=1; i<keyspec.size();i++) {
                            if (!keyspec[i].IsSequence() || (keyspec[i].size() != 2)) { return false; }

                            attrName=keyspec[i][0].as<std::string>();
                            attrType=keyspec[i][1].as<std::string>();
                            pythonString+=","+attrName+":"+attrType;
                            clusteringKeys.push_back(std::pair<std::string,std::string>(attrName,mapUserType(attrType)));
                        }
                        pythonString+=">";
                    } else {
                        throw ModuleException("Missing 'KeySpec' in specification");
                    }

                    if (node["ValueSpec"]) {
                        const Node valuespec =  node["ValueSpec"];
                        if (!valuespec.IsSequence() || (valuespec.size() == 0)) { return false; }
                        for (uint32_t i=0; i<valuespec.size();i++) {
                            if (!valuespec[i].IsSequence() || (valuespec[i].size() != 2)) { return false; }
                            attrName=valuespec[i][0].as<std::string>();
                            attrType=valuespec[i][1].as<std::string>();
                            pythonString+=","+attrName+":"+attrType;
                            cols.push_back(std::pair<std::string,std::string>(attrName,mapUserType(attrType)));
                        }
                        pythonString+=">\n";
                    } else {
                        throw ModuleException("Missing 'ValueSpec' in specification");
                    }

                    if (node["stream"]) {
                        const Node stream =  node["stream"];
                        pythonString+="   @stream\n";
                        streamEnabled = true;
                    }
                    pythonString+="   '''\n";

                } else if (typespec[1].as<std::string>() == "StorageObject") {
                    pythonString="from hecuba import StorageObj\n\n" + pythonString;
                    pythonString=pythonString+" (StorageObj):\n   '''\n";
                    obj_type=ObjSpec::valid_types::STORAGEOBJ_TYPE;
                    const Node classfields =  node["ClassField"];
                    if (!classfields.IsSequence() || (classfields.size() == 0)) { return false; }
                    partitionKeys.push_back(std::pair<std::string,std::string>("storage_id","uuid"));

                    for (uint32_t i=0; i<classfields.size();i++) {
                        if (!classfields[i].IsSequence() || (classfields[i].size() != 2)) { return false; }
                        attrName=classfields[i][0].as<std::string>();
                        attrType=classfields[i][1].as<std::string>();
                        pythonString+="   @ClassField "+attrName+" "+attrType+"\n";
                        cols.push_back(std::pair<std::string,std::string>(attrName,mapUserType(attrType)));
                    }
                    pythonString+="   '''\n";

                } else if (typespec[1].as<std::string>() == "StorageNumpy") {
                    pythonString="from hecuba import StorageNumpy\n\n" + pythonString;
                    pythonString += " (StorageNumpy):\n";
                    pythonString += "   '''\n";
                    obj_type=ObjSpec::valid_types::STORAGENUMPY_TYPE;
                    if (node["stream"]) {
                        const Node stream =  node["stream"];
                        pythonString += "   @stream\n";
                        streamEnabled = true;
                    }
                    pythonString += "   '''\n";
                } else {
                    DBG("HecubaSession: decode: Parsed 0: " << typespec[0].as<std::string>() << " Parsed 1: "<< typespec[1].as<std::string>());
                    return false;
                }
                DBG( " GENERATED: " << pythonString );
                dmodel.o=ObjSpec(obj_type,partitionKeys,clusteringKeys,cols,pythonString);
                if (streamEnabled) {
                    dmodel.o.enableStream();
                }
                return true;
            }
        };
};

std::vector<std::string> HecubaSession::split (std::string s, std::string delimiter) const{
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find (delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

/** contact_names_2_IP_addr: Given a string with a list of comma separated of
 * hostnames, returns a string with same hosts as IP address */
std::string HecubaSession::contact_names_2_IP_addr(std::string &contact_names)
const {
    std::vector<std::string> contact;
    std::vector<std::string> contact_ips;

    struct addrinfo hints;
    struct addrinfo *result;

    // Split contact_names
    contact = split(contact_names, ",");
    if (contact.size()==0) {
        fprintf(stderr, "Empty contact_names ");
        return std::string("");
    }


    /* Obtain address(es) matching host/port */

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */


    for (uint32_t i=0; i<contact.size(); i++) {
        int s = getaddrinfo(contact[i].c_str(), NULL, &hints, &result);
        if (s != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
            return std::string("");
        }
        /* getaddrinfo() returns a list of address structures.
           Try each address until we successfully connect(2).
           If socket(2) (or connect(2)) fails, we (close the socket
           and) try the next address. */

        if (result == NULL) {               /* No address succeeded */
            std::cerr<<"Address "<<contact[i]<<" is invalid\n"<<std::endl;
            return std::string("");
        }

        char host[NI_MAXHOST];
        if (getnameinfo(result->ai_addr, result->ai_addrlen
                    , host, sizeof(host)
                    , NULL, 0
                    , NI_NUMERICHOST) != 0) {
            std::cerr<<"Address "<<contact[i]<<" unable to get IP address: "<<strerror(errno)<<std::endl;
            return std::string("");
        }
        DBG("Address "<<contact[i]<<" translated to " <<std::string(host));
        contact_ips.push_back(host);

        freeaddrinfo(result);           /* No longer needed */

    }
    std::string contactips_str(contact_ips[0]);
    for (uint32_t i=1; i<contact_ips.size(); i++) {
        contactips_str+= "," + contact_ips[i];
    }
    return contactips_str;
}

void HecubaSession::parse_environment(config_map &config) {
    const char * nodePort = std::getenv("NODE_PORT");
    if (nodePort == nullptr) {
        nodePort = "9042";
    }
    config["node_port"] = std::string(nodePort);

    const char * contactNames = std::getenv("CONTACT_NAMES");
    if (contactNames == nullptr) {
        contactNames = "127.0.0.1";
    }
    // Transform Names to IP addresses (Cassandra's fault: cassandra_query_set_host needs an IP number)
    std::string cnames = std::string(contactNames);
    config["contact_names"] = contact_names_2_IP_addr(cnames);



    const char * kafkaNames = std::getenv("KAFKA_NAMES");
    if (kafkaNames == nullptr) {
        kafkaNames = contactNames;
    }
    config["kafka_names"] = std::string(kafkaNames);

    const char * createSchema = std::getenv("CREATE_SCHEMA");
    std::string createSchema2 ;
    if (createSchema == nullptr) {
        createSchema2 = std::string("true");
    } else {
        createSchema2 = std::string(createSchema);
        std::transform(createSchema2.begin(), createSchema2.end(), createSchema2.begin(),
                [](unsigned char c){ return std::tolower(c); });
    }
    config["create_schema"] = createSchema2;

    const char * executionName = std::getenv("EXECUTION_NAME");
    if (executionName == nullptr) {
        executionName = "my_app";
    }
    config["execution_name"] = std::string(executionName);

    const char * timestampedWrites = std::getenv("TIMESTAMPED_WRITES");
    std::string timestampedWrites2;
    if (timestampedWrites == nullptr) {
        timestampedWrites2 = std::string("false");
    } else {
        timestampedWrites2 = std::string(timestampedWrites);
        std::transform(timestampedWrites2.begin(), timestampedWrites2.end(), timestampedWrites2.begin(),
                [](unsigned char c){ return std::tolower(c); });
    }
    config["timestamped_writes"] = timestampedWrites2;

        //{"writer_buffer",      std::to_string(writer_queue)},??? == WRITE_BUFFER_SIZE?
    const char * writeBufferSize = std::getenv("WRITE_BUFFER_SIZE");
    if (writeBufferSize == nullptr) {
        writeBufferSize = "1000";
    }
    config["write_buffer_size"] = std::string(writeBufferSize);

        ///writer_par ==> 'WRITE_CALLBACKS_NUMBER'
    const char *writeCallbacksNum = std::getenv("WRITE_CALLBACKS_NUMBER");
    if (writeCallbacksNum == nullptr) {
        writeCallbacksNum = "16";
    }
    config["write_callbacks_number"] = std::string(writeCallbacksNum);

    const char * cacheSize = std::getenv("MAX_CACHE_SIZE");
    if (cacheSize == nullptr) {
        cacheSize = "1000";
    }
    config["max_cache_size"] = std::string(cacheSize);

    const char *replicationFactor = std::getenv("REPLICA_FACTOR");
    if (replicationFactor == nullptr) {
        replicationFactor = "1";
    }
    config["replica_factor"] = std::string(replicationFactor);

    const char *replicationStrategy = std::getenv("REPLICATION_STRATEGY");
    if (replicationStrategy == nullptr) {
        replicationStrategy = "SimpleStrategy";
    }
    config["replication_strategy"] = std::string(replicationStrategy);

    const char *replicationStrategyOptions = std::getenv("REPLICATION_STRATEGY_OPTIONS");
    if (replicationStrategyOptions == nullptr) {
        replicationStrategyOptions = "";
    }
    config["replication_strategy_options"] = replicationStrategyOptions;

    if (config["replication_strategy"] == "SimpleStrategy") {
        config["replication"] = std::string("{'class' : 'SimpleStrategy', 'replication_factor': ") + config["replica_factor"] + "}";
    } else {
        config["replication"] = std::string("{'class' : '") + config["replication_strategy"] + "', " + config["replication_strategy_options"] + "}";
    }
}

CassError HecubaSession::run_query(std::string query) const{
	CassStatement *statement = cass_statement_new(query.c_str(), 0);

    //std::cout << "DEBUG: HecubaSession.run_query : "<<query<<std::endl;
    CassFuture *result_future = cass_session_execute(const_cast<CassSession *>(storageInterface->get_session()), statement);
    cass_statement_free(statement);

    CassError rc = cass_future_error_code(result_future);
    if (rc != CASS_OK) {
        printf("Query execution error: %s - %s\n", cass_error_desc(rc), query.c_str());
    }
    cass_future_free(result_future);
    return rc;
}

void HecubaSession::decodeNumpyMetadata(HecubaSession::NumpyShape *s, void* metadata) {
    // Numpy Metadata(all unsigned): Ndims + Dim1 + Dim2 + ... + DimN  (Layout in C by default)
    unsigned* value = (unsigned*)metadata;
    s->ndims = *(value);
    value ++;
    s->dim = (unsigned *)malloc(s->ndims);
    for (unsigned i = 0; i < s->ndims; i++) {
        s->dim[i] = *(value + i);
    }
}
void HecubaSession::getMetaData(void * raw_numpy_meta, ArrayMetadata &arr_metas) {
    std::vector <uint32_t> dims;
    std::vector <uint32_t> strides;

    // decode void *metadatas
    HecubaSession:: NumpyShape * s = new HecubaSession::NumpyShape();
    decodeNumpyMetadata(s, raw_numpy_meta);
    uint32_t acum=1;
    for (uint32_t i=0; i < s->ndims; i++) {
        dims.push_back( s->dim[i]);
        acum *= s->dim[i];
    }
    for (uint32_t i=0; i < s->ndims; i++) {
        strides.push_back(acum * sizeof(double));
        acum /= s->dim[s->ndims-1-i];
    }
    uint32_t flags=NPY_ARRAY_C_CONTIGUOUS | NPY_ARRAY_WRITEABLE | NPY_ARRAY_ALIGNED;

    arr_metas.dims = dims;
    arr_metas.strides = strides;
    arr_metas.elem_size = sizeof(double);
    arr_metas.flags = flags;
    arr_metas.partition_type = ZORDER_ALGORITHM;
    arr_metas.typekind = 'f';
    arr_metas.byteorder = '=';
}

/* ASYNCHRONOUS */
void HecubaSession::registerNumpy(ArrayMetadata &numpy_meta, std::string name, uint64_t* uuid) {

    //std::cout<< "DEBUG: HecubaSession::registerNumpy BEGIN "<< name << UUID::UUID2str(uuid)<<std::endl;
    void *keys = std::malloc(sizeof(uint64_t *));
    uint64_t *c_uuid = (uint64_t *) malloc(sizeof(uint64_t) * 2);//new uint64_t[2];
    c_uuid[0] = *uuid;
    c_uuid[1] = *(uuid + 1);


    std::memcpy(keys, &c_uuid, sizeof(uint64_t *));


    char *c_name = (char *) std::malloc(name.length() + 1);
    std::memcpy(c_name, name.c_str(), name.length() + 1);

    //COPY VALUES
    int offset = 0;
    uint64_t size_name = strlen(c_name)+1;
    uint64_t size = 0;

    //size of the vector of dims
    size += sizeof(uint32_t) * numpy_meta.dims.size();

    //plus the other metas
    size += sizeof(numpy_meta.elem_size)
		+ sizeof(numpy_meta.partition_type)
		+ sizeof(numpy_meta.flags)
		+ sizeof(uint32_t)*numpy_meta.strides.size() // dims & strides
		+ sizeof(numpy_meta.typekind)
		+ sizeof(numpy_meta.byteorder);

    //allocate plus the bytes counter
    unsigned char *byte_array = (unsigned char *) malloc(size+ sizeof(uint64_t));
    unsigned char *name_array = (unsigned char *) malloc(size_name);


    // copy table name
    memcpy(name_array, c_name, size_name); //lgarrobe

    // Copy num bytes
    memcpy(byte_array+offset, &size, sizeof(uint64_t));
    offset += sizeof(uint64_t);


    //copy everything from the metas
	//	flags int, elem_size int, partition_type tinyint,
    //       dims list<int>, strides list<int>, typekind text, byteorder text


    memcpy(byte_array + offset, &numpy_meta.flags, sizeof(numpy_meta.flags));
    offset += sizeof(numpy_meta.flags);

    memcpy(byte_array + offset, &numpy_meta.elem_size, sizeof(numpy_meta.elem_size));
    offset += sizeof(numpy_meta.elem_size);

    memcpy(byte_array + offset, &numpy_meta.partition_type, sizeof(numpy_meta.partition_type));
    offset += sizeof(numpy_meta.partition_type);

    memcpy(byte_array + offset, &numpy_meta.typekind, sizeof(numpy_meta.typekind));
    offset +=sizeof(numpy_meta.typekind);

    memcpy(byte_array + offset, &numpy_meta.byteorder, sizeof(numpy_meta.byteorder));
    offset +=sizeof(numpy_meta.byteorder);

    memcpy(byte_array + offset, numpy_meta.dims.data(), sizeof(uint32_t) * numpy_meta.dims.size());
    offset +=sizeof(uint32_t)*numpy_meta.dims.size();

    memcpy(byte_array + offset, numpy_meta.strides.data(), sizeof(uint32_t) * numpy_meta.strides.size());
    offset +=sizeof(uint32_t)*numpy_meta.strides.size();

    //memcpy(byte_array + offset, &numpy_meta.inner_type, sizeof(numpy_meta.inner_type));
    //offset += sizeof(numpy_meta.inner_type);

    int offset_values = 0;
    char *values = (char *) malloc(sizeof(char *)*4);

    uint64_t *base_numpy = (uint64_t *) malloc(sizeof(uint64_t) * 2);//new uint64_t[2];
    memcpy(base_numpy, c_uuid, sizeof(uint64_t)*2);
    //std::cout<< "DEBUG: HecubaSession::registerNumpy &base_numpy = "<<base_numpy<<std::endl;
    std::memcpy(values, &base_numpy, sizeof(uint64_t *));  // base_numpy
    offset_values += sizeof(unsigned char *);

    char *class_name=(char*)malloc(strlen("hecuba.hnumpy.StorageNumpy")+1);
    strcpy(class_name, "hecuba.hnumpy.StorageNumpy");
    //std::cout<< "DEBUG: HecubaSession::registerNumpy &class_name = "<<class_name<<std::endl;
    memcpy(values+offset_values, &class_name, sizeof(unsigned char *)); //class_name
    offset_values += sizeof(unsigned char *);

    //std::cout<< "DEBUG: HecubaSession::registerNumpy &name = "<<name_array<<std::endl;
    memcpy(values+offset_values, &name_array, sizeof(unsigned char *)); //name
    offset_values += sizeof(unsigned char *);

    //std::cout<< "DEBUG: HecubaSession::registerNumpy &np_meta = "<<byte_array<<std::endl;
    memcpy(values+offset_values, &byte_array,  sizeof(unsigned char *)); // numpy_meta
    offset_values += sizeof(unsigned char *);

    try {
        numpyMetaWriter->write_to_cassandra(keys, values);
        numpyMetaWriter->wait_writes_completion(); // Ensure hecuba.istorage get all updates SYNCHRONOUSLY (to avoid race conditions with poll that may request a build_remotely on this new object)!
    }
    catch (std::exception &e) {
        std::cerr << "HecubaSession::registerNumpy: Error writing" <<std::endl;
        std::cerr << e.what();
        throw e;
    };

}

void HecubaSession::createSchema(void) {
    // Create hecuba
    std::vector<std::string> queries;
    std::string create_hecuba_keyspace = std::string(
            "CREATE KEYSPACE IF NOT EXISTS hecuba  WITH replication = ") +  config["replication"];
    queries.push_back(create_hecuba_keyspace);
    std::string create_hecuba_qmeta = std::string(
            "CREATE TYPE IF NOT EXISTS hecuba.q_meta("
            "mem_filter text,"
            "from_point frozen<list<double>>,"
            "to_point frozen<list<double>>,"
            "precision float);");
    queries.push_back(create_hecuba_qmeta);
    std::string create_hecuba_npmeta = std::string(
            "CREATE TYPE IF NOT EXISTS hecuba.np_meta ("
            "flags int, elem_size int, partition_type tinyint,"
            "dims list<int>, strides list<int>, typekind text, byteorder text)");
    queries.push_back(create_hecuba_npmeta);
    std::string create_hecuba_istorage = std::string(
            "CREATE TABLE IF NOT EXISTS hecuba.istorage"
            "(storage_id uuid,"
            "class_name text,name text,"
            "istorage_props map<text,text>,"
            "tokens list<frozen<tuple<bigint,bigint>>>,"
            "indexed_on list<text>,"
            "qbeast_random text,"
            "qbeast_meta frozen<q_meta>,"
            "numpy_meta frozen<np_meta>,"
            "block_id int,"
            "base_numpy uuid,"
            "view_serialization blob,"
            "primary_keys list<frozen<tuple<text,text>>>,"
            "columns list<frozen<tuple<text,text>>>,"
            "PRIMARY KEY(storage_id));");
    queries.push_back(create_hecuba_istorage);
    // Create keyspace EXECUTION_NAME
    std::string create_keyspace = std::string(
            "CREATE KEYSPACE IF NOT EXISTS ") + config["execution_name"] +
        std::string(" WITH replication = ") +  config["replication"];
    queries.push_back(create_keyspace);

    for(auto q: queries) {
        CassError rc = run_query(q);
        if (rc != CASS_OK) {
            std::string msg = std::string("HecubaSession:: Error Creating Schema executing: ") + q;
            throw ModuleException(msg);
        }
    }
}

/***************************
 * PUBLIC
 ***************************/

/* Constructor: Establish connection with underlying storage system */
HecubaSession::HecubaSession() : currentDataModel(NULL) {

    parse_environment(this->config);



    /* Establish connection */
    this->storageInterface = std::make_shared<StorageInterface>(stoi(config["node_port"]), config["contact_names"]);
    //this->storageInterface = new StorageInterface(stoi(config["node_port"]), config["contact_names"]);

    if (this->config["create_schema"] == "true") {
        createSchema();
    }

// TODO: extend writer to support lists	std::vector<config_map> pkeystypes = { {{"name", "storage_id"}} };
// TODO: extend writer to support lists	std::vector<config_map> ckeystypes = {};
// TODO: extend writer to support lists	std::vector<config_map> colstypes = {{{"name", "class_name"}},
// TODO: extend writer to support lists							{{"name", "name"}},
// TODO: extend writer to support lists							{{"name", "tokens"}},   //list
// TODO: extend writer to support lists							{{"name", "primary_keys"}}, //list
// TODO: extend writer to support lists							{{"name", "columns"}},  //list
// TODO: extend writer to support lists							{{"name", "indexed_on"}} }; //list
// TODO: extend writer to support lists	dictMetaWriter = storageInterface->make_writer("istorage", "hecuba",
// TODO: extend writer to support lists													pkeystypes, colstypes,
// TODO: extend writer to support lists													config);
// TODO: extend writer to support lists


std::vector<config_map> pkeystypes_n = { {{"name", "storage_id"}} };
std::vector<config_map> ckeystypes_n = {};
std::vector<config_map> colstypes_n = {
						{{"name", "base_numpy"}},
                        {{"name", "class_name"}},
						{{"name", "name"}},
						{{"name", "numpy_meta"}},
						//{{"name", "block_id"}}, //NOT USED
						//{{"name", "view_serialization"}},  //Used by Python, uses pickle format. Let it be NULL and initialized at python code
};
						// TODO: extend writer to support lists {{"name", "tokens"}} }; //list

// The attributes stored in istorage for all numpys are the same, we use a single writer for the session
numpyMetaAccess = storageInterface->make_cache("istorage", "hecuba",
												pkeystypes_n, colstypes_n,
												config);
numpyMetaWriter = numpyMetaAccess->get_writer();

}

HecubaSession::~HecubaSession() {
    delete(currentDataModel);
    delete(numpyMetaAccess);
}

/* loadDataModel: loads a DataModel from 'model_filename' path which should be
 * a YAML file. It also generates its corresponding python generated class in
 * the same directory where the model resides or in the 'pythonSpecPath'
 * directory. */
void HecubaSession::loadDataModel(const char * model_filename, const char * pythonSpecPath) {

    if (currentDataModel != NULL) {
        std::cerr << "WARNING: HecubaSession::loadDataModel: DataModel already defined. Discarded and load again"<<std::endl;
        delete(currentDataModel);
    }

    // TODO: parse file to get model information NOW HARDCODED

    // class dataModel(StorageDict):
    //    '''
    //         @TypeSpec dict <<lat:int,ts:int>,metrics:numpy.ndarray>
    //    '''


    DataModel* d = new DataModel();


    if (model_filename == NULL) {
		throw std::runtime_error("Trying to load a NULL model file name ");
    }

    // Detect dirname and basename for 'model_filename'
    std::string pythonPath("");
    std::string baseName(model_filename);

    size_t pos = baseName.find_last_of('/');
    if (pos != std::string::npos) { // model_filename has a path
        if (pythonSpecPath != NULL) {
            pythonPath = pythonSpecPath;
        } else {
            pythonPath = baseName.substr(0,pos);
        }
        baseName = baseName.substr(pos+1, baseName.size());
    }
    // Remove .yaml extension
    pos = baseName.find_last_of('.');
    if (pos != std::string::npos) {
        baseName = baseName.substr(0, pos);
    }
    // Create moduleName
    std::string moduleName;
    if (pythonPath == "") {
        moduleName = baseName;
    } else {
        moduleName = pythonPath + "/" + baseName;
    }

    d->setModuleName(moduleName);

    // Add .py extension to moduleName
    std::ofstream fd(moduleName+".py");

    YAML::Node node = YAML::LoadFile(model_filename);

    assert(node.Type() == YAML::NodeType::Sequence);

    for(std::size_t i=0;i<node.size();i++) {
		DataModel::datamodel_spec x = node[i].as<DataModel::datamodel_spec>();
		d->addObjSpec(moduleName + "." + x.id, x.o);
        fd<<x.o.getPythonString();
    }
    fd.close();

    // ALWAYS Add numpy (just in case) ##############################
    std::vector<std::pair<std::string, std::string>> pkeystypes_numpy = {
                                  {"storage_id", "uuid"},
                                  {"cluster_id", "int"}
    };
    std::vector<std::pair<std::string, std::string>> ckeystypes_numpy = {{"block_id","int"}};
    std::vector<std::pair<std::string, std::string>> colstypes_numpy = {
                                  {"payload", "blob"},
    };
    d->addObjSpec(ObjSpec::valid_types::STORAGENUMPY_TYPE, "hecuba.hnumpy.StorageNumpy", pkeystypes_numpy, ckeystypes_numpy, colstypes_numpy);
    // ##############################

    currentDataModel = d;
}

/* Given a class name 'id_model' returns its Fully Qualified Name with Python
 * Style using the current Data Model modulename.
 * Examples:
 *      classname --> modulename.classname
 *      hecuba.hnumpy.StorageNumpy --> hecuba.hnumpy.StorageNumpy
 *      path1.path2.classname -> NOT SUPPORTED YET (should be the same)
 */
std::string HecubaSession::getFQname(const char* id_model) const {
    std::string FQid_model (id_model);
    if (strcmp(id_model, "hecuba.hnumpy.StorageNumpy")==0) {
        // Special treatment for NUMPY
        FQid_model = "hecuba.hnumpy.StorageNumpy";

    } else if (FQid_model.find_first_of(".") ==  std::string::npos) {
        // FQid_model: Fully Qualified name for the id_model: module_name.id_model
        //      In YAML we allow to define the class_name without the model:
        //          file: model_complex.py
        //             class info (StorageObj):
        //                  ...
        //      But we store the Fully Qualified name> "model_complex.info"
        FQid_model.insert(0, currentDataModel->getModuleName() + ".");

    }
    return FQid_model;
}

/* Given a FQname return a name suitable to be stored as a tablename in Cassandra */
std::string HecubaSession::getTableName(std::string FQname) const {
    // FIXME: We currently only allow classes from a unique
    // model, because just the class name is stored in cassandra
    // without any reference to the modulename. An option could be
    // to store the modulename and the classname separated by '_'.
    // For now, we just keep the classname as tablename
    std::string table_name (FQname);
    int pos = table_name.find_last_of(".");
    table_name = table_name.substr(pos+1);
    return table_name;
}

IStorage* HecubaSession::createObject(const char * id_model, uint64_t* uuid) {
    // Instantitate an existing object

    DataModel* model = currentDataModel;
    if (model == NULL) {
        throw ModuleException("HecubaSession::createObject No data model loaded");
    }

    // FQid_model: Fully Qualified name for the id_model: module_name.id_model
    //      In YAML we allow to define the class_name without the model:
    //          file: model_complex.py
    //             class info (StorageObj):
    //                  ...
    //      But we store the Fully Qualified name> "model_complex.info"
    std::string FQid_model = getFQname(id_model);

    IStorage * o;
    ObjSpec oType = model->getObjSpec(FQid_model);
    DBG(" HecubaSession::createObject INSTANTIATING " << oType.debug());
    switch(oType.getType()) {
        case ObjSpec::valid_types::STORAGEOBJ_TYPE:
        case ObjSpec::valid_types::STORAGEDICT_TYPE:
            {
                // read from istorage: uuid --> id_object (name)
                // A new buffer for the uuid (key) is needed (Remember that
                // retrieve_from_cassandra creates a TupleRow of the parameter
                // and therefore the parameter can NOT be a stack pointer... as
                // it will be freed on success)
                void * localuuid = malloc(2*sizeof(uint64_t));
                memcpy(localuuid, uuid, 2*sizeof(uint64_t));
                void * key = malloc(sizeof(char*));
                memcpy(key, &localuuid, sizeof(uint64_t*));

                std::vector <const TupleRow*> result = numpyMetaAccess->retrieve_from_cassandra(key);

                if (result.empty()) throw ModuleException("HecubaSession::createObject uuid "+UUID::UUID2str(uuid)+" not found. Unable to instantiate");

                uint32_t pos = numpyMetaAccess->get_metadata()->get_columnname_position("name");
                char *keytable = *(char**)result[0]->get_element(pos); //Value retrieved from cassandra has 'keyspace.tablename' format

                std::string keyspace (keytable);
                std::string tablename;
                pos = keyspace.find_first_of('.');
                tablename = keyspace.substr(pos+1);
                keyspace = keyspace.substr(0,pos);

                const char * id_object = tablename.c_str();

                // Check that retrieved classname form hecuba coincides with 'id_model'
                pos = numpyMetaAccess->get_metadata()->get_columnname_position("class_name");
                char *classname = *(char**)result[0]->get_element(pos); //Value retrieved from cassandra has 'keyspace.tablename' format
                std::string sobj_table_name (classname);

                // The class_name retrieved in the case of the storageobj is
                // the fully qualified name, but in cassandra the instances are
                // stored in a table with the name of the last part(example:
                // "model_complex.info" will have instances in "keyspace.info")
                // meaning that in a complex scenario with different models...
                // we will loose information. FIXME
                if (sobj_table_name.compare(FQid_model) != 0) {
                    throw ModuleException("HecubaSession::createObject uuid "+UUID::UUID2str(uuid)+" "+ keytable + " has unexpected class_name " + sobj_table_name + " instead of "+FQid_model);
                }



                //  Create Writer for storageobj
                std::vector<config_map>* keyNamesDict = oType.getKeysNamesDict();
                std::vector<config_map>* colNamesDict = oType.getColsNamesDict();

                CacheTable *dataAccess = NULL;
                if (oType.getType() == ObjSpec::valid_types::STORAGEOBJ_TYPE) {
                    dataAccess = storageInterface->make_cache(getTableName(FQid_model).c_str(), keyspace.c_str(),
                            *keyNamesDict, *colNamesDict,
                            config);
                } else {
                    dataAccess = storageInterface->make_cache(id_object, keyspace.c_str(),
                            *keyNamesDict, *colNamesDict,
                            config);
                }
                delete keyNamesDict;
                delete colNamesDict;

                // IStorage needs a UUID pointer... but the parameter 'uuid' is
                // from the user, therefore we can not count on it
                localuuid = malloc(2*sizeof(uint64_t));
                memcpy(localuuid, uuid, 2*sizeof(uint64_t));

                o = new IStorage(this, FQid_model, keyspace + "." + id_object, (uint64_t*)localuuid, dataAccess);

                if (oType.isStream()) {
                    std::string topic = std::string(UUID::UUID2str(uuid));
                    DBG("     AND IT IS AN STREAM!");
                    o->enableStream(topic);
                }
            }
            break;
        case ObjSpec::valid_types::STORAGENUMPY_TYPE:
            {
                // read from istorage: uuid --> metadata and id_object
                // A new buffer for the uuid (key) is needed (Remember that
                // retrieve_from_cassandra creates a TupleRow of the parameter
                // and therefore the parameter can NOT be a stack pointer... as
                // it will be freed on success)
                void * localuuid = malloc(2*sizeof(uint64_t));
                memcpy(localuuid, uuid, 2*sizeof(uint64_t));
                void * key = malloc(sizeof(char*));
                memcpy(key, &localuuid, sizeof(uint64_t*));

                std::vector <const TupleRow*> result = numpyMetaAccess->retrieve_from_cassandra(key);

                if (result.empty()) throw ModuleException("HecubaSession::createObject uuid "+UUID::UUID2str(uuid)+" not found. Unable to instantiate");

                uint32_t pos = numpyMetaAccess->get_metadata()->get_columnname_position("name");
                char *keytable = *(char**)result[0]->get_element(pos); //Value retrieved from cassandra has 'keyspace.tablename' format

                std::string keyspace (keytable);
                std::string tablename;
                pos = keyspace.find_first_of('.');
                tablename = keyspace.substr(pos+1);
                keyspace = keyspace.substr(0,pos);

                // Read the UDT case (numpy_meta)from the row retrieved from cassandra
                pos = numpyMetaAccess->get_metadata()->get_columnname_position("numpy_meta");
                ArrayMetadata *numpy_metas = *(ArrayMetadata**)result[0]->get_element(pos);
                DBG("DEBUG: HecubaSession::createNumpy . Size "<< numpy_metas->get_array_size());

                // StorageNumpy
                ArrayDataStore *array_store = new ArrayDataStore(tablename.c_str(), keyspace.c_str(),
                        this->storageInterface->get_session(), config);
                //std::cout << "DEBUG: HecubaSession::createObject After ArrayDataStore creation " <<std::endl;

                // IStorage needs a UUID pointer... but the parameter 'uuid' is
                // from the user, therefore we can not count on it
                localuuid = malloc(2*sizeof(uint64_t));
                memcpy(localuuid, uuid, 2*sizeof(uint64_t));

                o = new IStorage(this, FQid_model, keytable, (uint64_t*)localuuid, array_store->getWriteCache());
                o->setNumpyAttributes(array_store, *numpy_metas); // SET METAS and DATA!!
                if (oType.isStream()) {
                    std::string topic = std::string(UUID::UUID2str(uuid));
                    DBG("     AND IT IS AN STREAM!");
                    o->enableStream(topic);
                }
            }
            break;
        default:
            throw ModuleException("HECUBA Session: createObject Unknown type ");// + std::string(oType.objtype));
            break;
    }
    return o;
}

IStorage* HecubaSession::createObject(const char * id_model, const char * id_object, void * metadata, void* value) {
    // Create Cassandra tables 'ksp.id_object' for object 'id_object' according to its type 'id_model' in 'model'

    DataModel* model = currentDataModel;
    if (model == NULL) {
        throw ModuleException("HecubaSession::createObject No data model loaded");
    }

    std::string FQid_model = getFQname(id_model);

    IStorage * o;
    ObjSpec oType = model->getObjSpec(FQid_model);
    //std::cout << "DEBUG: HecubaSession::createObject '"<<FQid_model<< "' ==> " <<oType.debug()<<std::endl;

    std::string id_object_str;
    if (id_object == nullptr) { //No name used, generate a new one
        id_object_str = "X" + UUID::UUID2str(UUID::generateUUID()); //Cassandra does NOT like to have a number at the beginning of a table name
        std::replace(id_object_str.begin(), id_object_str.end(), '-','_'); //Cassandra does NOT like character '-' in table names
    } else {
        id_object_str = std::string(id_object);
    }
    std::string name(config["execution_name"] + "." + id_object_str);
    uint64_t *c_uuid = UUID::generateUUID5(name.c_str()); // UUID for the new object

    switch(oType.getType()) {
        case ObjSpec::valid_types::STORAGEOBJ_TYPE:
            {
                // StorageObj case
                //  Create table 'class_name' "CREATE TABLE ksp.class_name (storage_id UUID, nom typ, ... PRIMARY KEY (storage_id))"
                std::string query = "CREATE TABLE IF NOT EXISTS " +
                    config["execution_name"] + "." + id_model +
                    oType.table_attr;

                CassError rc = run_query(query);
                if (rc != CASS_OK) {
                    if (rc == CASS_ERROR_SERVER_INVALID_QUERY) { // keyspace does not exist
                        std::cout<< "HecubaSession::createObject: Keyspace "<< config["execution_name"]<< " not found. Creating keyspace." << std::endl;
                        std::string create_keyspace = std::string(
                                "CREATE KEYSPACE IF NOT EXISTS ") + config["execution_name"] +
                            std::string(" WITH replication = ") +  config["replication"];
                        rc = run_query(create_keyspace);
                        if (rc != CASS_OK) {
                            std::string msg = std::string("HecubaSession:: Error executing query ") + create_keyspace;
                            throw ModuleException(msg);
                        } else {
                            rc = run_query(query); // Repeat table creation after creating keyspace
                            if (rc != CASS_OK) {
                                std::string msg = std::string("HecubaSession:: Error executing query ") + query;
                                throw ModuleException(msg);
                            }
                        }
                    } else {
                        std::string msg = std::string("HecubaSession:: Error executing query ") + query;
                        throw ModuleException(msg);
                    }
                }
                // Table for storageobj class created
                // Add entry to ISTORAGE: TODO add the tokens attribute
                std::string insquery = std::string("INSERT INTO ") +
                    std::string("hecuba.istorage") +
                    std::string("(storage_id, name, class_name, columns)") +
                    std::string("VALUES ") +
                    std::string("(") +
                    UUID::UUID2str(c_uuid) + std::string(", ") +
                    "'" + name + "'" + std::string(", ") +
                    "'" + FQid_model + "'" + std::string(", ") +
                    oType.getColsStr() +
                    std::string(")");
                run_query(insquery);

                //  Create Writer for storageobj
                std::vector<config_map>* keyNamesDict = oType.getKeysNamesDict();
                std::vector<config_map>* colNamesDict = oType.getColsNamesDict();

                CacheTable *dataAccess = storageInterface->make_cache(getTableName(FQid_model).c_str(), config["execution_name"].c_str(),
                          *keyNamesDict, *colNamesDict,
                          config);
                delete keyNamesDict;
                delete colNamesDict;
                o = new IStorage(this, FQid_model, config["execution_name"] + "." + id_object_str, c_uuid, dataAccess);

            }
            break;
        case ObjSpec::valid_types::STORAGEDICT_TYPE:
            {
                // Dictionary case
                //  Create table 'name' "CREATE TABLE ksp.name (nom typ, nom typ, ... PRIMARY KEY (nom, nom))"
                bool new_element = true;
                std::string query = "CREATE TABLE " +
                    config["execution_name"] + "." + id_object_str +
                    oType.table_attr;

                CassError rc = run_query(query);
                if (rc != CASS_OK) {
                    if (rc == CASS_ERROR_SERVER_ALREADY_EXISTS ) {
                        new_element = false; //OOpps, creation failed. It is an already existent object.
                    } else if (rc == CASS_ERROR_SERVER_INVALID_QUERY) {
                        std::cout<< "HecubaSession::createObject: Keyspace "<< config["execution_name"]<< " not found. Creating keyspace." << std::endl;
                        std::string create_keyspace = std::string(
                                "CREATE KEYSPACE IF NOT EXISTS ") + config["execution_name"] +
                            std::string(" WITH replication = ") +  config["replication"];
                        rc = run_query(create_keyspace);
                        if (rc != CASS_OK) {
                            std::string msg = std::string("HecubaSession::createObject: Error creating keyspace ") + create_keyspace;
                            throw ModuleException(msg);
                        } else {
                            rc = run_query(query);
                            if (rc != CASS_OK) {
                                if (rc == CASS_ERROR_SERVER_ALREADY_EXISTS) {
                                    new_element = false; //OOpps, creation failed. It is an already existent object.
                                }  else {
                                    std::string msg = std::string("HecubaSession::createObject: Error executing query ") + query;
                                    throw ModuleException(msg);
                                }
                            }
                        }
                    } else {
                        std::string msg = std::string("HecubaSession::createObject: Error executing query ") + query;
                        throw ModuleException(msg);
                    }
                }

                if (new_element) {
                    //  Add entry to hecuba.istorage: TODO add the tokens attribute
                    // TODO EXPECTED:vvv NOW HARDCODED
                    //classname = FQid_model
                    // keys = {c_uuid}, values={name, class_name, primary_keys, columns } # no tokens, no numpy_meta, ...
                    //try {
                    //	dictMetaWriter->write_to_cassandra(keys, values);
                    //}
                    //catch (std::exception &e) {
                    //	std::cerr << "Error writing in registering" <<std::endl;
                    //	std::cerr << e.what();
                    //	throw e;
                    //}
                    // EXPECTED^^^^
                    // Example: insert into hecuba.istorage (storage_id, primary_keys) values (3dd30d5d-b0d4-45b5-a21a-c6ad313007fd, [('lat','int'),('ts','int')]);
                    std::string insquery = std::string("INSERT INTO ") +
                        std::string("hecuba.istorage") +
                        std::string("(storage_id, name, class_name, primary_keys, columns)") +
                        std::string("VALUES ") +
                        std::string("(") +
                        UUID::UUID2str(c_uuid) + std::string(", ") +
                        "'" + name + "'" + std::string(", ") +
                        "'" + FQid_model + "'" + std::string(", ") +
                        oType.getKeysStr() + std::string(", ") +
                        oType.getColsStr() +
                        std::string(")");
                    run_query(insquery);
                } else {
                    std::cerr << "WARNING: Object "<<id_object_str<<" already exists. Trying to overwrite it. It may fail if the schema does not match."<<std::endl;
                    // TODO: THIS IS NOT WORKING. We need to get the storage_id (c_uuid) from istorage DISABLE
                    // TODO: Check the schema in Cassandra matches the model
                }

                //  Create Writer for dictionary
                std::vector<config_map>* keyNamesDict = oType.getKeysNamesDict();
                std::vector<config_map>* colNamesDict = oType.getColsNamesDict();


                std::string topic = std::string(UUID::UUID2str(c_uuid));

                CacheTable *reader = storageInterface->make_cache(id_object_str.c_str(), config["execution_name"].c_str(),
                        *keyNamesDict, *colNamesDict,
                        config);

                delete keyNamesDict;
                delete colNamesDict;
                o = new IStorage(this, FQid_model, config["execution_name"] + "." + id_object_str, c_uuid, reader);
                DBG("HecubaSession::createObject: CREATED NEW STORAGEDICT with uuid "<< topic);
                if (oType.isStream()) {
                    DBG("     AND IT IS AN STREAM!");
                    o->enableStream(topic);
                }
            }
            break;

        case ObjSpec::valid_types::STORAGENUMPY_TYPE:
            {
                // Create table
                std::string query = "CREATE TABLE IF NOT EXISTS " + config["execution_name"] + "." + id_object_str +
                    " (storage_id uuid, cluster_id int, block_id int, payload blob, "
                    "PRIMARY KEY((storage_id,cluster_id),block_id)) "
                    "WITH compaction = {'class': 'SizeTieredCompactionStrategy', 'enabled': false};";

                this->run_query(query);

                // StorageNumpy
                ArrayDataStore *array_store = new ArrayDataStore(id_object_str.c_str(), config["execution_name"].c_str(),
                        this->storageInterface->get_session(), config);
                //std::cout << "DEBUG: HecubaSession::createObject After ArrayDataStore creation " <<std::endl;

                // Create entry in hecuba.istorage for the new numpy
                ArrayMetadata numpy_metas;
                getMetaData(metadata, numpy_metas); // numpy_metas = getMetaData(metadata);
                DBG("DEBUG: HecubaSession::createNumpy . Size "<< numpy_metas.get_array_size());
                //std::cout<< "DEBUG: HecubaSession::createObject After metadata creation " <<std::endl;

                registerNumpy(numpy_metas, name, c_uuid);

                //std::cout<< "DEBUG: HecubaSession::createObject After REGISTER numpy into ISTORAGE" <<std::endl;

                //Create keys, values to store the numpy
                //double* tmp = (double*)value;
                //std::cout<< "DEBUG: HecubaSession::createObject BEFORE store FIRST element in NUMPY is "<< *tmp << " and second is "<<*(tmp+1)<< " 3rd " << *(tmp+2)<< std::endl;
                array_store->store_numpy_into_cas(c_uuid, numpy_metas, value);

                o = new IStorage(this, FQid_model, config["execution_name"] + "." + id_object_str, c_uuid, array_store->getWriteCache());
                std::string topic = std::string(UUID::UUID2str(c_uuid));
                DBG("HecubaSession::createObject: CREATED NEW STORAGENUMPY with uuid "<< topic);
                o->setNumpyAttributes(array_store, numpy_metas,value);
                if (oType.isStream()) {
                    DBG("     AND IT IS AN STREAM!");
                    o->enableStream(topic);
                }

            }
            break;
        default:
            throw ModuleException("HECUBA Session: createObject Unknown type ");// + std::string(oType.objtype));
            break;
    }
    //std::cout << "DEBUG: HecubaSession::createObject DONE" << std::endl;
    return o;
}


DataModel* HecubaSession::getDataModel() {
    return currentDataModel;
}
