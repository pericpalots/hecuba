#include "TupleRowFactory.h"
#include "debug.h"

// StringValue is used for easy comparing numpy metas
enum StringValue {
	evNotDefined,
	evFlags,
	evElemSize,
	evPartitionType,
	evTypeKind,
	evByteOrder,
	evDims,
	evStrides
};

// Map to associate the strings with the enum values
static std::map<std::string, StringValue> s_mapStringValues;

static void initialize_mapStringValues()
{
  s_mapStringValues["flags"] = evFlags;
  s_mapStringValues["elem_size"] = evElemSize;
  s_mapStringValues["partition_type"] = evPartitionType;
  s_mapStringValues["typekind"] = evTypeKind;
  s_mapStringValues["byteorder"] = evByteOrder;
  s_mapStringValues["dims"] = evDims;
  s_mapStringValues["strides"] = evStrides;
}
/***
 * Builds a tuple factory to retrieve tuples based on rows and keys
 * extracting the information from Cassandra to decide the types to be used
 * @param table_meta Holds the table information
 */
TupleRowFactory::TupleRowFactory(std::shared_ptr<const std::vector<ColumnMeta> > row_info) {
    this->metadata = row_info;
    this->total_bytes = 0;
    if (row_info->end() != row_info->begin()) {
        std::vector<ColumnMeta>::const_iterator last_element = --row_info->end();
        total_bytes = last_element->position + last_element->size;
    }
	initialize_mapStringValues();
}



/*** TUPLE BUILDERS ***/

/***
 * Build a tuple taking the given buffer as its internal representation
 * @param data Valid pointer to the values
 * @return TupleRow with the buffer as its inner data
 * @post The TupleRow now owns the data and this cannot be freed
 */
TupleRow *TupleRowFactory::make_tuple(void *data) const {
    return new TupleRow(metadata, total_bytes, data);
}

/***
 * Build a tuple from the given Cassandra result row using the factory's metadata
 * @param row Contains the same number of columns than the metadata
 * @return TupleRow with a copy of the values inside the row
 * @post The row can be freed
 */
TupleRow *TupleRowFactory::make_tuple(const CassRow *row) {
    if (!row) return NULL;
    char *buffer = nullptr;

    if (total_bytes > 0) buffer = (char *) malloc(total_bytes);

    TupleRow *new_tuple = new TupleRow(metadata, total_bytes, buffer);

    CassIterator *it = cass_iterator_from_row(row);
    for (uint16_t i = 0; cass_iterator_next(it) && i < metadata->size(); ++i) {
        if (cass_to_c(cass_iterator_get_column(it), buffer + metadata->at(i).position, i) == -1) {
            new_tuple->setNull(i);
        }
    }
    cass_iterator_free(it);
    return new_tuple;
}

//build a tuple row for just one value of a cassandra row
//we need this to return just one column
TupleRow *TupleRowFactory::make_tuple(const CassValue *value) {
    char *buffer = nullptr;
    if (total_bytes > 0) buffer = (char *) malloc(total_bytes);

    TupleRow *new_tuple = new TupleRow(metadata, total_bytes, buffer);

    if (cass_to_c(value, buffer, 0) == -1) {
        new_tuple->setNull(0);
    }

    return new_tuple;
}



/* setArrayMetadataField - Initializes field from and ArrayMetaData using data
 *     from a CassValue.
 * Args:
 * 'np_metas'	Destination ArrayMetaData to modify
 * 'field'		Field name to modify
 * 'field_name_length' 	Length of 'field'
 * 'field_value'	Source CassValue to extract 'field' */
void TupleRowFactory::setArrayMetadataField(ArrayMetadata &np_metas, const char *field, size_t field_name_length, const CassValue* field_value) const {
    int32_t int_value;
    int8_t short_value;

    // Use a map and a switch to avoid a sequence of 'strcmp'
    std::string field_name (field);
    auto it = s_mapStringValues.find(field_name);
    if (it == s_mapStringValues.end()) {
        throw ModuleException("TupleRowFactory::setArrayMetadataField Found unknown field named "+field_name+". Exitting.");
    }
    switch (it->second) {
        case evFlags: //field_name  = "flags";
            cass_value_get_int32(field_value,&int_value);
            np_metas.flags=int_value;
            DBG("TupleRowFactory::setArrayMetadataField flags "<<int_value);
            break;
        case evElemSize: //field_name  = "elem_size";
            cass_value_get_int32(field_value,&int_value);
            np_metas.elem_size = int_value;
            DBG("TupleRowFactory::setArrayMetadataField elem_size "<<int_value);
            break;
        case evPartitionType: // field_name  = "partition_type";
            cass_value_get_int8(field_value,&short_value);
            np_metas.partition_type = short_value;
            DBG("TupleRowFactory::setArrayMetadataField partitiontype "<<short_value);
            break;
        case evTypeKind: { //field_name  = "typekind";
                             const char * string_value;
                             size_t int_value;
                             cass_value_get_string(field_value,&string_value, &int_value);
                             if (int_value>1) {
                                 throw ModuleException("TupleRowFactory::cass_to_c Unexpected TypeKind. Length should be 1 but received "+int_value);
                             }
                             np_metas.typekind = string_value[0];
                             DBG("TupleRowFactory::setArrayMetadataField typekind "<<string_value);
                         }
                         break;
        case evByteOrder: { //field_name  = "byteorder";
                              const char * string_value;
                              size_t int_value;
                              cass_value_get_string(field_value,&string_value, &int_value);
                              if (int_value>1) {
                                  throw ModuleException("TupleRowFactory::cass_to_c Unexpected ByteOrder. Length should be 1 but received "+int_value);
                              }
                              np_metas.byteorder = string_value[0];
                             DBG("TupleRowFactory::setArrayMetadataField byteorder "<<string_value);
                          }
                          break;
        case evDims: {//field_name="dims";
                        CassIterator* it = cass_iterator_from_collection(field_value);
                        while (cass_iterator_next(it)) {
                            const CassValue* val = cass_iterator_get_value(it);
                            cass_value_get_int32(val, &int_value);
                            np_metas.dims.push_back(int_value);
                            DBG("TupleRowFactory::setArrayMetadataField dims "<<int_value);
                        }
                     }
                     break;
        case evStrides: {//field_name="strides";
                        CassIterator* it = cass_iterator_from_collection(field_value);
                        while (cass_iterator_next(it)) {
                            const CassValue* val = cass_iterator_get_value(it);
                            cass_value_get_int32(val, &int_value);
                            np_metas.strides.push_back(int_value);
                            DBG("TupleRowFactory::setArrayMetadataField strides "<<int_value);
                        }
                     }
                     break;
        default:
                     throw ModuleException("TupleRowFactory::cass_to_c Unexpected field value ["+field_name+"]");
    }
}

/***
 * @pre: -
 * @post: Extract the Cassandra's value from lhs and writes it to the memory pointed by data
 * using the data type information provided by type_array[col]
 * @param lhs Cassandra value
 * @param data Pointer to the place where the extracted value "lhs" should be written
 * @param col Indicates which column, thus, which data type are we processing
 * @return 0 if succeeds
 */
int TupleRowFactory::cass_to_c(const CassValue *lhs, void *data, int16_t col) const {

    if (col < 0 || col >= (int32_t) metadata->size()) {
        throw ModuleException("TupleRowFactory: Cass to C: Asked for column " + std::to_string(col) + " but only " +
                              std::to_string(metadata->size()) + " are present");
    }
    switch (metadata->at(col).type) {
        case CASS_VALUE_TYPE_TEXT:
        case CASS_VALUE_TYPE_VARCHAR:
        case CASS_VALUE_TYPE_ASCII: {
            const char *l_temp;
            size_t l_size;
            CassError rc = cass_value_get_string(lhs, &l_temp, &l_size);
            CHECK_CASS("TupleRowFactory: Cassandra to C parse text unsuccessful, column" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            char *permanent = (char *) malloc(l_size + 1);
            memcpy(permanent, l_temp, l_size);
            permanent[l_size] = '\0';
            memcpy(data, &permanent, sizeof(char *));
            return 0;
        }
        case CASS_VALUE_TYPE_VARINT:
        case CASS_VALUE_TYPE_BIGINT: {
            int64_t *p = static_cast<int64_t * >(data);
            CassError rc = cass_value_get_int64(lhs, p);
            CHECK_CASS(
                    "TupleRowFactory: Cassandra to C parse bigint/varint unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_BLOB: {
            const unsigned char *l_temp;
            size_t l_size;
            CassError rc = cass_value_get_bytes(lhs, &l_temp, &l_size);
            CHECK_CASS("TupleRowFactory: Cassandra to C parse bytes unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;

            //Allocate space for the bytes
            char *permanent = (char *) malloc(l_size + sizeof(uint64_t));
            //TODO make sure l_size < uint32 max
            uint64_t int_size = (uint64_t) l_size;

            //copy num bytes
            memcpy(permanent, &int_size, sizeof(uint64_t));

            //copy bytes
            memcpy(permanent + sizeof(uint64_t), l_temp, l_size);

            //copy pointer to payload
            memcpy(data, &permanent, sizeof(char *));
            return 0;
        }
        case CASS_VALUE_TYPE_BOOLEAN: {
            cass_bool_t b;
            CassError rc = cass_value_get_bool(lhs, &b);
            CHECK_CASS("TupleRowFactory: Cassandra to C parse bool unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            bool *p = static_cast<bool *>(data);
            if (b == cass_true) *p = true;
            else *p = false;
            return 0;
        }
        case CASS_VALUE_TYPE_COUNTER: {
            CassError rc = cass_value_get_uint32(lhs, reinterpret_cast<uint32_t *>(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse counter as uint32 unsuccessful, column:" +
                       std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_DOUBLE: {
            CassError rc = cass_value_get_double(lhs, reinterpret_cast<double *>(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse double unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_FLOAT: {
            CassError rc = cass_value_get_float(lhs, reinterpret_cast<float * >(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse float unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_INT: {
            CassError rc = cass_value_get_int32(lhs, reinterpret_cast<int32_t * >(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse int32 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_UUID: {
            CassUuid uuid;
            CassError rc = cass_value_get_uuid(lhs, &uuid);

            CHECK_CASS("TupleRowFactory: Cassandra to C parse UUID unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            char *permanent = (char *) malloc(sizeof(uint64_t) * 2);
            cassuuid2uuid(uuid, &permanent);
            memcpy(data, &permanent, sizeof(char *));
            return 0;
        }
        case CASS_VALUE_TYPE_SMALL_INT: {
            CassError rc = cass_value_get_int16(lhs, reinterpret_cast<int16_t * >(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse int16 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_TINY_INT: {
            CassError rc = cass_value_get_int8(lhs, reinterpret_cast<int8_t * >(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse int16 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_TUPLE: {
            if (metadata->at(col).pointer->empty())
                throw ModuleException("TupleRowFactory: Cassandra to C parse tuple unsuccessful, tuple is empty");

            TupleRowFactory TFACT = TupleRowFactory(metadata->at(col).pointer);

            void *tuple_data = malloc(TFACT.get_nbytes());

            TupleRow **ptr = (TupleRow **) data;
            *ptr = TFACT.make_tuple(tuple_data);

            CassIterator *tuple_iterator = cass_iterator_from_tuple(lhs);
            if (!tuple_iterator)
                throw ModuleException(
                        "TupleRowFactory: Cassandra to C parse tuple unsuccessful, data type is not tuple");

            /* Iterate over the tuple fields */
            uint32_t j = 0;
            const CassValue *value;
            char *pos_to_copy;
            while (cass_iterator_next(tuple_iterator)) {
                value = cass_iterator_get_value(tuple_iterator);
                pos_to_copy = (char *) tuple_data + metadata->at(col).pointer->at(j).position;
                if (TFACT.cass_to_c(value, pos_to_copy, j) < 0) {
                    TupleRow *inner_data = *ptr;
                    inner_data->setNull(j);
                }
                ++j;
            }
            /* The tuple iterator needs to be freed */
            cass_iterator_free(tuple_iterator);
            return 0;
        }
        case CASS_VALUE_TYPE_DATE: {
            cass_uint32_t year_month_day;
            CassError rc = cass_value_get_uint32(lhs, &year_month_day);
            CHECK_CASS("TupleRowFactory: Cassandra to C parse uint32 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            int64_t time = (int64_t) cass_date_time_to_epoch(year_month_day, 0);
            memcpy(data, &time, sizeof(int64_t));
            return 0;
        }
        case CASS_VALUE_TYPE_TIME: {
            CassError rc = cass_value_get_int64(lhs, reinterpret_cast<int64_t * >(data));
            CHECK_CASS("TupleRowFactory: Cassandra to C parse int64 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            return 0;
        }
        case CASS_VALUE_TYPE_TIMESTAMP: {
            cass_int64_t time_of_day;
            CassError rc = cass_value_get_int64(lhs, &time_of_day);
            CHECK_CASS("TupleRowFactory: Cassandra to C parse int64 unsuccessful, column:" + std::to_string(col));
            if (rc == CASS_ERROR_LIB_NULL_VALUE) return -1;
            memcpy(data, &time_of_day, sizeof(int64_t));
            return 0;
        }
        case CASS_VALUE_TYPE_UDT: {
            DBG(" TupleRowFactory::cass_to_c: WE ARE IN THE CASS_VALUE_TYPE_UDT ");
            if (cass_value_is_null(lhs)) return -1;
            // Iterate items inside UDT
            // https://docs.datastax.com/en/developer/cpp-driver/2.16/topics/basics/user_defined_types/

            ArrayMetadata *np_metas = new ArrayMetadata(); // Dummy ArrayMetadata to store temporal values

            /* Create an iterator for the UDT value */
            CassIterator* udt_iterator = cass_iterator_fields_from_user_type(lhs);

            /* Iterate over the UDT fields */
            while (cass_iterator_next(udt_iterator)) {
                const char* field_name;
                size_t field_name_length;
                /* Get UDT field name */
                cass_iterator_get_user_type_field_name(udt_iterator,
                        &field_name, &field_name_length);

                /* Get UDT field value */
                const CassValue* field_value =
                    cass_iterator_get_user_type_field_value(udt_iterator);

                setArrayMetadataField(*np_metas, field_name, field_name_length, field_value);

            }

            /* The UDT iterator needs to be freed */
            cass_iterator_free(udt_iterator);

            memcpy(data, &np_metas, sizeof(char*));


            return 0;
        }
        case CASS_VALUE_TYPE_DECIMAL:
        case CASS_VALUE_TYPE_TIMEUUID:
        case CASS_VALUE_TYPE_INET:
        case CASS_VALUE_TYPE_LIST:
        case CASS_VALUE_TYPE_MAP:
        case CASS_VALUE_TYPE_SET:
        case CASS_VALUE_TYPE_CUSTOM:
        case CASS_VALUE_TYPE_UNKNOWN:
        default:
            throw ModuleException("Default behaviour not supported");
    }
}

void
TupleRowFactory::bind(CassTuple *tuple, const TupleRow *row) const {

    if (!row || !tuple)
        throw ModuleException("Tuple bind: Null tuple received");

    if (metadata->size() != row->n_elem())
        throw ModuleException("Tuple bind: Found " + std::to_string(row->n_elem()) + ", expected " +
                              std::to_string(metadata->size()));

    for (uint16_t bind_pos = 0; bind_pos < row->n_elem(); ++bind_pos) {
        const void *element_i = row->get_element(bind_pos);
        if (element_i != nullptr && !row->isNull(bind_pos)) {
            switch (metadata->at(bind_pos).type) {
                case CASS_VALUE_TYPE_VARCHAR:
                case CASS_VALUE_TYPE_TEXT:
                case CASS_VALUE_TYPE_ASCII: {
                    int64_t *addr = (int64_t *) element_i;
                    const char *d = reinterpret_cast<char *>(*addr);
                    CassError rc = cass_tuple_set_string(tuple, (size_t) bind_pos, d);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Varchar/Text/ASCII to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_VARINT:
                case CASS_VALUE_TYPE_BIGINT: {
                    int64_t *value = (int64_t *) element_i;
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Varint/Bigint to the tuple");
                    break;

                }
                case CASS_VALUE_TYPE_BLOB: {
                    int64_t *value = (int64_t *) element_i;
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Blob to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_BOOLEAN: {
                    cass_bool_t *value = (cass_bool_t *) element_i;
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Boolean to tuple");
                    break;
                }
                    //TODO parsed as uint32 or uint64 on different methods
                case CASS_VALUE_TYPE_COUNTER: {
                    int64_t *value = (int64_t *) element_i;
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Counter to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_DOUBLE: {
                    double_t *value = (double_t *) element_i;
                    CassError rc = cass_tuple_set_double(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Double to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_FLOAT: {
                    float_t *value = (float_t *) element_i;
                    CassError rc = cass_tuple_set_float(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Float to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_INT: {
                    int32_t *value = (int32_t *) element_i;
                    CassError rc = cass_tuple_set_int32(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Int to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_UUID: {
                    const uint64_t **uuid = (const uint64_t **) element_i;

                    CassUuid cass_uuid;
                    uuid2cassuuid(uuid, cass_uuid);

                    CassError rc = cass_tuple_set_uuid(tuple, (size_t) bind_pos, cass_uuid);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding Uuid to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_SMALL_INT: {
                    int16_t *value = (int16_t *) element_i;
                    CassError rc = cass_tuple_set_int16(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding SmallInt to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_TINY_INT: {
                    int8_t *value = (int8_t *) element_i;
                    CassError rc = cass_tuple_set_int8(tuple, (size_t) bind_pos, *value);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding TinyInt to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_TUPLE: {
                    TupleRow **ptr = (TupleRow **) element_i;
                    const TupleRow *inner_data = *ptr;
                    TupleRowFactory TFACT = TupleRowFactory(metadata->at(bind_pos).pointer);
                    unsigned long n_types = metadata->at(bind_pos).pointer->size();

                    CassTuple *new_tuple = cass_tuple_new(n_types);
                    TFACT.bind(new_tuple, inner_data);
                    CassError rc = cass_tuple_set_tuple(tuple, (size_t) bind_pos, new_tuple);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding a new Tuple to the existing tuple");
                    break;
                }
                case CASS_VALUE_TYPE_DATE: {
                    const time_t time = *((time_t *) element_i);
                    uint32_t year_month_day = cass_date_from_epoch(time);
                    CassError rc = cass_tuple_set_uint32(tuple, (size_t) bind_pos, year_month_day);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding int64 to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_TIME: {
                    int64_t time = *((int64_t *) element_i);
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, time);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding int64 to the tuple");
                    break;
                }
                case CASS_VALUE_TYPE_TIMESTAMP: {
                    cass_int64_t time = *((int64_t *) element_i);
                    CassError rc = cass_tuple_set_int64(tuple, (size_t) bind_pos, time);
                    CHECK_CASS("TupleRowFactory: Cassandra unsuccessful binding int64 to the tuple");
                    break;

                }
                case CASS_VALUE_TYPE_DECIMAL:
                case CASS_VALUE_TYPE_TIMEUUID:
                case CASS_VALUE_TYPE_INET:
                case CASS_VALUE_TYPE_LIST:
                case CASS_VALUE_TYPE_MAP:
                case CASS_VALUE_TYPE_SET:
                case CASS_VALUE_TYPE_UDT:
                case CASS_VALUE_TYPE_CUSTOM:
                case CASS_VALUE_TYPE_UNKNOWN:
                default:
                    throw ModuleException("Default behaviour not supported");
            }
        } else {
            //Element is a nullptr
            CassError rc = cass_tuple_set_null(tuple, bind_pos);
            CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [Null value]");
        }
    }
}


/*
    Encode a RFC4122 UUID format into a CassUUID(which uses Little endian)
    Args:
       uuid: RFC4122 UUID format BIGENDIAN
*/
void
TupleRowFactory::uuid2cassuuid(const uint64_t** uuid, CassUuid& cass_uuid) const {
    const uint64_t *time_and_version = *uuid;
    const uint64_t *clock_seq_and_node = *uuid + 1;

    char *p = (char*)&cass_uuid.time_and_version;
    char *psrc = (char*)time_and_version;
    // Recode time_low
    p[0] = psrc[3];
    p[1] = psrc[2];
    p[2] = psrc[1];
    p[3] = psrc[0];

    // Recode time_mid
    p[4] = psrc[5];
    p[5] = psrc[4];

    // Recode time_hi_&_version
    p[6] = psrc[7];
    p[7] = psrc[6];

    // Recode clock_seq_and_node
    p = (char*)&cass_uuid.clock_seq_and_node;
    psrc = (char*)clock_seq_and_node;

    for (uint32_t ix=0; ix<8;ix++)
        p[ix] = psrc[7-ix];
}

void
TupleRowFactory::cassuuid2uuid(const CassUuid& cass_uuid, char** uuid) const {
    char *p = (*uuid);
    char *psrc = (char*)&cass_uuid.time_and_version;
    // Recode time_low
    p[0] = psrc[3];
    p[1] = psrc[2];
    p[2] = psrc[1];
    p[3] = psrc[0];

    // Recode time_mid
    p[4] = psrc[5];
    p[5] = psrc[4];

    // Recode time_hi_&_version
    p[6] = psrc[7];
    p[7] = psrc[6];

    // Recode clock_seq_and_node
    p= (*uuid + 8);
    psrc = (char*)&cass_uuid.clock_seq_and_node;

    for (uint32_t ix=0; ix<8;ix++)
        p[ix] = psrc[7-ix];

}

void
TupleRowFactory::bind(CassStatement *statement, const TupleRow *row, u_int16_t offset) const {


    if (!row || !statement)
        throw ModuleException("Statement bind: Null tuple row or statement received");

    if (metadata->size() != row->n_elem())
        throw ModuleException("Statement bind: Found " + std::to_string(row->n_elem()) + ", expected " +
                              std::to_string(metadata->size()));

    for (uint16_t i = 0; i < row->n_elem(); ++i) {
        uint16_t bind_pos = offset + i;
        const void *element_i = row->get_element(i);
        if (element_i != nullptr && !row->isNull(i)) {
            switch (metadata->at(i).type) {
                case CASS_VALUE_TYPE_VARCHAR:
                case CASS_VALUE_TYPE_TEXT:
                case CASS_VALUE_TYPE_ASCII: {
                    int64_t *addr = (int64_t *) element_i;
                    const char *d = reinterpret_cast<char *>(*addr);
                    CassError rc = cass_statement_bind_string(statement, bind_pos, d);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [text], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_VARINT:
                case CASS_VALUE_TYPE_BIGINT: {
                    const int64_t *data = static_cast<const int64_t *>(element_i);
                    CassError rc = cass_statement_bind_int64(statement, bind_pos,
                                                             *data);//L means long long, K unsigned long long
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [bigint/varint], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_BLOB: {
                    unsigned char *byte_array;
                    byte_array = *(unsigned char **) element_i;
                    uint64_t *num_bytes = (uint64_t *) byte_array;
                    const unsigned char *bytes = byte_array + sizeof(uint64_t);
                    cass_statement_bind_bytes(statement, bind_pos, bytes, *num_bytes);
                    break;
                }
                case CASS_VALUE_TYPE_BOOLEAN: {
                    cass_bool_t b = cass_false;
                    const bool *bindbool = static_cast<const bool *>(element_i);

                    if (*bindbool) b = cass_true;
                    CassError rc = cass_statement_bind_bool(statement, bind_pos, b);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [bool], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                    //TODO parsed as uint32 or uint64 on different methods
                case CASS_VALUE_TYPE_COUNTER: {
                    const uint64_t *data = static_cast<const uint64_t *>(element_i);
                    CassError rc = cass_statement_bind_int64(statement, bind_pos,
                                                             *data);//L means long long, K unsigned long long
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [counter as uint64], column:" +
                               metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_DOUBLE: {
                    const double *data = static_cast<const double *>(element_i);
                    CassError rc = cass_statement_bind_double(statement, bind_pos, *data);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [double], column:" +
                               metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_FLOAT: {
                    const float *data = static_cast<const float *>(element_i);

                    CassError rc = cass_statement_bind_float(statement, bind_pos, *data);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [float], column:" +
                               metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_INT: {
                    const int32_t *data = static_cast<const int32_t *>(element_i);

                    CassError rc = cass_statement_bind_int32(statement, bind_pos, *data);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [int32], column:" +
                               metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_UUID: {
                    const uint64_t **uuid = (const uint64_t **) element_i;

                    CassUuid cass_uuid;
                    uuid2cassuuid(uuid, cass_uuid);

                    char myfinal[CASS_UUID_STRING_LENGTH];
                    cass_uuid_string(cass_uuid, myfinal);

                    CassError rc = cass_statement_bind_uuid(statement, bind_pos, cass_uuid);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UUID], column:" +
                               metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_SMALL_INT: {
                    const int16_t *data = static_cast<const int16_t *>(element_i);
                    CassError rc = cass_statement_bind_int16(statement, bind_pos, *data);
                    CHECK_CASS(
                            "TupleRowFactory: Cassandra binding query unsuccessful [small int as int16], column:" +
                            metadata->at(i).info.begin()->second);

                    break;
                }
                case CASS_VALUE_TYPE_TINY_INT: {
                    const int8_t *data = static_cast<const int8_t *>(element_i);
                    CassError rc = cass_statement_bind_int8(statement, bind_pos, *data);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [tiny int as int8], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_TUPLE: {
                    TupleRow **ptr = (TupleRow **) element_i;
                    const TupleRow *inner_data = *ptr;
                    TupleRowFactory TFACT = TupleRowFactory(metadata->at(i).pointer);
                    unsigned long n_types = metadata->at(i).pointer->size();
                    CassTuple *tuple = cass_tuple_new(n_types);
                    TFACT.bind(tuple, inner_data);
                    cass_statement_bind_tuple(statement, bind_pos, tuple);
                    cass_tuple_free(tuple);
                    break;
                }
                case CASS_VALUE_TYPE_DATE: {
                    const time_t time = *((time_t *) element_i);
                    cass_uint32_t year_month_day = cass_date_from_epoch(time);
                    CassError rc = cass_statement_bind_uint32(statement, bind_pos, year_month_day);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [date as int64], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_TIME: {
                    int64_t time = *((int64_t *) element_i);
                    CassError rc = cass_statement_bind_int64(statement, bind_pos, time);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [time as int64], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_TIMESTAMP: {
                    cass_int64_t time = *((int64_t *) element_i);
                    CassError rc = cass_statement_bind_int64(statement, bind_pos, time);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [timestamp as int64], column:" +
                               metadata->at(i).info.begin()->second);
                    break;
                }
                case CASS_VALUE_TYPE_UDT: {
                    int64_t *addr = *((int64_t **) element_i);
                    int64_t size = *addr;
                    addr=(int64_t *)(((char *) addr)+sizeof(int64_t));
                    CassError rc;

                    CassUserType* cass_np_meta = cass_user_type_new_from_data_type(metadata->at(i).dtype);

                    ////////
                    // NOTE: The code here MUST match the code in 'MetaManager.register_obj'
                    ////////

                    ArrayMetadata np_metas = ArrayMetadata(); // Dummy ArrayMetadata to store temporal values

                    /* Minimum size of ArrayMetaData. 'sizeof' can not be used as the compiler may add some padding */


                    int64_t sizeof_ArrayMetaData = sizeof(np_metas.elem_size)
                            + sizeof(np_metas.partition_type)
                            + sizeof(np_metas.flags)
                            + sizeof(np_metas.typekind)
                            + sizeof(np_metas.byteorder);
                    if (size < sizeof_ArrayMetaData) {
                            throw ModuleException("Corrupted data. Data does not fit ArrayMetaData");
                    }
                    int offset = 0;
                    unsigned char *byte_array = reinterpret_cast<unsigned char*>(addr);
                    memcpy(&np_metas.flags, byte_array + offset, sizeof(np_metas.flags));
                    offset += sizeof(np_metas.flags);
                    memcpy(&np_metas.elem_size, byte_array + offset, sizeof(np_metas.elem_size));
                    offset += sizeof(np_metas.elem_size);
                    memcpy(&np_metas.partition_type, byte_array + offset, sizeof(np_metas.partition_type));
                    offset += sizeof(np_metas.partition_type);
                    memcpy(&np_metas.typekind, byte_array + offset, sizeof(np_metas.typekind));
                    offset += sizeof(np_metas.typekind);
                    memcpy(&np_metas.byteorder, byte_array + offset, sizeof(np_metas.byteorder));
                    offset += sizeof(np_metas.byteorder);
                    // The remaining elements will be read later to avoid the recreation the vectors                        
                    uint32_t remain = (size - offset)/sizeof(uint32_t);

                    if ((remain <= 0) || ((remain % 2) != 0)) {
                            throw ModuleException("Corrupted data. Data does not fit ArrayMetaData or even number of dims/strides");
                    }
                    uint32_t nelems = remain / 2;
                    std::string field_name;

                    field_name  = "flags";
                    rc = cass_user_type_set_int32_by_name(cass_np_meta, field_name.c_str(), np_metas.flags);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    field_name  = "elem_size";
                    rc = cass_user_type_set_int32_by_name(cass_np_meta, field_name.c_str(), np_metas.elem_size);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    field_name  = "partition_type";
                    rc = cass_user_type_set_int8_by_name(cass_np_meta, field_name.c_str(), np_metas.partition_type);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    field_name  = "typekind";
                    rc = cass_user_type_set_string_by_name_n(cass_np_meta, field_name.c_str(), field_name.length(), &np_metas.typekind, 1);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    field_name  = "byteorder";
                    rc = cass_user_type_set_string_by_name_n(cass_np_meta, field_name.c_str(), field_name.length(), &np_metas.byteorder, 1);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);

                    field_name="dims";
                    CassCollection* dims_collection = cass_collection_new(CASS_COLLECTION_TYPE_LIST, nelems);
                    for (uint32_t pos = 0; pos < nelems; pos++ ) {
                        uint32_t value;
                        memcpy(&value, byte_array + offset, sizeof(value));
                        offset += sizeof(value);
                        rc = cass_collection_append_int32(dims_collection, value);
                        CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    }

                    rc = cass_user_type_set_collection_by_name(cass_np_meta,field_name.c_str(),dims_collection);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    field_name="strides";
                    CassCollection* str_collection = cass_collection_new(CASS_COLLECTION_TYPE_LIST,nelems);
                    for (uint32_t pos = 0; pos < nelems; pos++ ) {
                           uint32_t value;
                           memcpy(&value, byte_array + offset, sizeof(value));
                        offset += sizeof(value);
                        rc = cass_collection_append_int32(str_collection, value);
                        CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);
                    }

                    rc = cass_user_type_set_collection_by_name(cass_np_meta,field_name.c_str(),str_collection);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], field:" + field_name);

                    rc = cass_statement_bind_user_type(statement, bind_pos, cass_np_meta);
                    CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [UDT], column:" +
                               metadata->at(i).info.begin()->second);


                    break;

                }
                case CASS_VALUE_TYPE_DECIMAL:
                case CASS_VALUE_TYPE_TIMEUUID:
                case CASS_VALUE_TYPE_INET:
                case CASS_VALUE_TYPE_LIST:
                case CASS_VALUE_TYPE_MAP:
                case CASS_VALUE_TYPE_SET:
                case CASS_VALUE_TYPE_CUSTOM:
                case CASS_VALUE_TYPE_UNKNOWN:
                default:
                    throw ModuleException("Default behaviour not supported");
            }
        } else {
            //Element is a nullptr
            CassError rc = cass_statement_bind_null(statement, bind_pos);
            CHECK_CASS("TupleRowFactory: Cassandra binding query unsuccessful [Null value], column:" +
                       metadata->at(i).info.begin()->second);
        }
    }
}

//const uint64_t get_content_size(const TupleRow* row) const {
std::vector <uint32_t> TupleRowFactory::get_content_sizes(const TupleRow* row) const {
    std::vector <uint32_t> elements_size(row->n_elem());
    if (!row)
        throw ModuleException(" get_content_size: Null tuple row received");

    if (metadata->size() != row->n_elem())
        throw ModuleException(" get_content_size: Found " + std::to_string(row->n_elem()) + ", expected " +
                              std::to_string(metadata->size()));

    for (uint16_t i = 0; i < row->n_elem(); ++i) {
        const void *element_i = row->get_element(i);
        if (element_i != nullptr && !row->isNull(i)) {
            switch (metadata->at(i).type) {
                case CASS_VALUE_TYPE_VARCHAR:
                case CASS_VALUE_TYPE_TEXT:
                case CASS_VALUE_TYPE_ASCII: {
                    int64_t *addr = (int64_t *) element_i;
                    const char *d = reinterpret_cast<char *>(*addr);
                    elements_size[i]=strlen(d)+1;
                    break;
                }
                case CASS_VALUE_TYPE_VARINT:
                case CASS_VALUE_TYPE_BIGINT: {
                    const int64_t *data = static_cast<const int64_t *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_BLOB: {
                    unsigned char *byte_array;
                    byte_array = *(unsigned char **) element_i;
                    uint64_t *num_bytes = (uint64_t *) byte_array;
                    elements_size[i]=*num_bytes+sizeof(uint64_t);
                    break;
                }
                case CASS_VALUE_TYPE_BOOLEAN: {
                    const bool *bindbool = static_cast<const bool *>(element_i);
                    elements_size[i]=sizeof(*bindbool);
                    break;
                }
                    //TODO parsed as uint32 or uint64 on different methods
                case CASS_VALUE_TYPE_COUNTER: {
                    const uint64_t *data = static_cast<const uint64_t *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_DOUBLE: {
                    const double *data = static_cast<const double *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_FLOAT: {
                    const float *data = static_cast<const float *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_INT: {
                    const int32_t *data = static_cast<const int32_t *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_UUID: {
                    //const uint64_t **uuid = (const uint64_t **) element_i;
                    elements_size[i]=sizeof(uint64_t)*2;
                    break;
                }
                case CASS_VALUE_TYPE_SMALL_INT: {
                    const int16_t *data = static_cast<const int16_t *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_TINY_INT: {
                    const int8_t *data = static_cast<const int8_t *>(element_i);
                    elements_size[i]=sizeof(*data);
                    break;
                }
                case CASS_VALUE_TYPE_TUPLE: {
                    TupleRow **ptr = (TupleRow **) element_i;
                    const TupleRow *inner_data = *ptr;
                    TupleRowFactory TFACT = TupleRowFactory(metadata->at(i).pointer);
                    uint64_t inner_size = TFACT.get_content_size(inner_data);
                    elements_size[i]=inner_size;
                    break;
                }
                case CASS_VALUE_TYPE_DATE: {
                    const time_t time = *((time_t *) element_i);
                    elements_size[i]=sizeof(time);
                    break;
                }
                case CASS_VALUE_TYPE_TIME: {
                    int64_t time = *((int64_t *) element_i);
                    elements_size[i]=sizeof(time);
                    break;
                }
                case CASS_VALUE_TYPE_TIMESTAMP: {
                    cass_int64_t time = *((int64_t *) element_i);
                    elements_size[i]=sizeof(time);
                    break;
                }
                case CASS_VALUE_TYPE_UDT: {
                    int64_t *addr = *((int64_t **) element_i);
                    int64_t size = *addr;
                    elements_size[i]=size+sizeof(int64_t);
                    break;

                }
                case CASS_VALUE_TYPE_DECIMAL:
                case CASS_VALUE_TYPE_TIMEUUID:
                case CASS_VALUE_TYPE_INET:
                case CASS_VALUE_TYPE_LIST:
                case CASS_VALUE_TYPE_MAP:
                case CASS_VALUE_TYPE_SET:
                case CASS_VALUE_TYPE_CUSTOM:
                case CASS_VALUE_TYPE_UNKNOWN:
                default:
                    throw ModuleException("Default behaviour not supported");
            }
        }
    }
    return elements_size;
}

const uint64_t TupleRowFactory::get_content_size(const TupleRow* row) const {
    std::vector<uint32_t> s = get_content_sizes(row);
    uint64_t res = 0;
    for (uint32_t i: s) {
        res += i;
    }
    return res;
}

void * TupleRowFactory::get_element_addr(const void *element_i, const uint16_t pos) const {
        void *result;
        if (element_i != nullptr ){
            switch (metadata->at(pos).type) {
                case CASS_VALUE_TYPE_VARCHAR:
                case CASS_VALUE_TYPE_TEXT:
                case CASS_VALUE_TYPE_ASCII:
                case CASS_VALUE_TYPE_DATE:
                case CASS_VALUE_TYPE_TIME:
                case CASS_VALUE_TYPE_TIMESTAMP:
                case CASS_VALUE_TYPE_UDT:
                case CASS_VALUE_TYPE_UUID:
                case CASS_VALUE_TYPE_TUPLE:
                case CASS_VALUE_TYPE_BLOB: {
                    result =  *(void **) element_i;
                    break;
                }
                case CASS_VALUE_TYPE_VARINT:
                case CASS_VALUE_TYPE_BIGINT:
                case CASS_VALUE_TYPE_BOOLEAN:
                case CASS_VALUE_TYPE_COUNTER:
                case CASS_VALUE_TYPE_DOUBLE:
                case CASS_VALUE_TYPE_FLOAT:
                case CASS_VALUE_TYPE_INT:
                case CASS_VALUE_TYPE_SMALL_INT:
                case CASS_VALUE_TYPE_TINY_INT: {
                    result = (void *)element_i;
                    break;
                }
                case CASS_VALUE_TYPE_DECIMAL:
                case CASS_VALUE_TYPE_TIMEUUID:
                case CASS_VALUE_TYPE_INET:
                case CASS_VALUE_TYPE_LIST:
                case CASS_VALUE_TYPE_MAP:
                case CASS_VALUE_TYPE_SET:
                case CASS_VALUE_TYPE_CUSTOM:
                case CASS_VALUE_TYPE_UNKNOWN:
                default:
                    throw ModuleException("Default behaviour not supported");
            }
        }
    return result;
}

/*
 * Encode a TupleRow to be sent by network. This means to encode a vector of
 * null values and the content of the TupleRow values.
 * Args:
 *  row     TupleRow to encode
 *  dest    Pointer where to store the encoded buffer (must have enough space for data + null_values vector)
 */
void TupleRowFactory::encode(const TupleRow *row, void *dest) const {
    if (row->n_elem() != n_elements()) {
        throw ModuleException("ERROR: Using a TupleRow with " + std::to_string(row->n_elem()) +" but expecting "+std::to_string(n_elements()));
    }

    // Allocate the encoded buffer plus the vector of null values
    uint32_t nullvalues_size = ceil(((double)row->n_elem())/32)*sizeof(uint32_t);

    // get_content_size should return a vector with the size of each element
    // we need to add r_factory to the writer 
    std::vector<uint32_t> element_sizes = this->get_content_sizes(row);

    uint64_t total_size;
    for (auto& n : element_sizes)
         total_size += n;

    // Copy the null values vector
    std::copy(row->get_null_values().begin(), row->get_null_values().end(), (char*)dest);
    DBG(" TupleRowFactory::encode "<<row->n_elem()<<"elements to buffer:"<<((void*)dest));

    uint64_t offset = nullvalues_size; // Skip the null values.

    void *src;
    for (uint16_t i = 0; i < row->n_elem(); ++i) {
        DBG(" TupleRowFactory::encode   pos:"<<std::to_string(i)<<" - null?"<<std::to_string(row->isNull(i))<<" offset:"<<offset);
        if (!row->isNull(i)) {
            // if the element is a tuple we call rescursively to copy_tuple_content
            const void *element_i = row->get_element(i);
            if (metadata->at(i).type == CASS_VALUE_TYPE_TUPLE) {
                TupleRow **ptr = (TupleRow **) element_i;
                const TupleRow *inner_data = *ptr;
                TupleRowFactory TFACT = TupleRowFactory(metadata->at(i).pointer);
                uint32_t inner_nullvalues_size = inner_data->get_null_values().size() * sizeof(uint32_t);
                src = malloc(TFACT.get_content_size(inner_data)+inner_nullvalues_size);
                TFACT.encode(inner_data, src);
            } else {
                src = get_element_addr(element_i, i);
                DBG(" TupleRowFactory::encode    ALLOC NEEDED src="<< src<< " size="<<element_sizes[i]);
            }
            memcpy(((char*)dest)+offset, src, element_sizes[i]);
            offset += element_sizes[i];
        }
    }
}

TupleRow * TupleRowFactory::decode(const void *encoded_buff) const {

    void *row_buffer = malloc(this->get_nbytes());
    char *p_row_buffer = (char *) row_buffer;

    uint64_t size=0;
    bool alloc = false;
    char * addr = (char *)encoded_buff;
    void *src;

    uint32_t nullvalues_size = ceil(((double)n_elements())/32)*sizeof(uint32_t);
    std::vector<uint32_t>null_values((char*)encoded_buff, ((char*)encoded_buff)+nullvalues_size);

    addr += nullvalues_size; // SKIP null values
    DBG(" TupleRowFactory::decode "<<n_elements()<<"elements from buffer:"<<((void*)encoded_buff));


    for (uint16_t i = 0; i < n_elements(); ++i) {
        DBG(" TupleRowFactory::decode   pos:"<<std::to_string(i)<<" - null?"<<std::to_string((null_values[i>>5]&(1<<i%32)))<<" addr:"<<(void*)addr);
        if ((null_values[i>>5] & (1 << i%32)) == 0) {
            switch (metadata->at(i).type) {
                case CASS_VALUE_TYPE_VARCHAR:
                case CASS_VALUE_TYPE_TEXT:
                case CASS_VALUE_TYPE_ASCII:{
                   size = strlen(addr) + 1;
                   alloc = true;
                   break;
                }
                case CASS_VALUE_TYPE_DATE:
                case CASS_VALUE_TYPE_TIME:
                case CASS_VALUE_TYPE_TIMESTAMP: {
                    alloc = true;
                    size = sizeof(time_t);
                    break;
                }
                case CASS_VALUE_TYPE_UUID:{
                    alloc = true;
                    size = sizeof(uint64_t) * 2;
                    break;
                }
                case CASS_VALUE_TYPE_TUPLE:{
                    /* we will allocate the size of the inner tupple */
                    alloc = true;
                    break;
                }
                case CASS_VALUE_TYPE_UDT:
                case CASS_VALUE_TYPE_BLOB: {
                     // first the size and then the content of the blob or the numpy
                     size = sizeof(uint64_t) + **((uint64_t **)addr);
                     alloc = true;
                     break;
                }
                case CASS_VALUE_TYPE_VARINT:
                case CASS_VALUE_TYPE_BIGINT:
                case CASS_VALUE_TYPE_BOOLEAN:
                case CASS_VALUE_TYPE_COUNTER:
                case CASS_VALUE_TYPE_DOUBLE:
                case CASS_VALUE_TYPE_FLOAT:
                case CASS_VALUE_TYPE_INT:
                case CASS_VALUE_TYPE_SMALL_INT:
                case CASS_VALUE_TYPE_TINY_INT: {
                     size=metadata->at(i).size;
                     alloc = false;
                     break;
                }
                case CASS_VALUE_TYPE_DECIMAL:
                case CASS_VALUE_TYPE_TIMEUUID:
                case CASS_VALUE_TYPE_INET:
                case CASS_VALUE_TYPE_LIST:
                case CASS_VALUE_TYPE_MAP:
                case CASS_VALUE_TYPE_SET:
                case CASS_VALUE_TYPE_CUSTOM:
                case CASS_VALUE_TYPE_UNKNOWN:
                default:
                        throw ModuleException("Default behaviour not supported");
            }
            if (alloc) {
                if (metadata->at(i).type == CASS_VALUE_TYPE_TUPLE) {
                    TupleRowFactory TFACT = TupleRowFactory(metadata->at(i).pointer);
                    TupleRow *innerdata = TFACT.make_tuple(addr);
                    size=TFACT.get_content_size(innerdata);
                    // a tuple cannot have nulls
                    src = TFACT.decode(addr)->get_payload();
                    if (src == NULL)
                        throw ModuleException("Tuple without content received");
                } else {
                    src = malloc(size);
                    memcpy(src, addr, size);
                    DBG(" TupleRowFactory::decode    ALLOC src="<< src<< " size="<<size);
                }
                memcpy(p_row_buffer,&src,sizeof(src));
                p_row_buffer += sizeof(src);
                addr += size;
            } else {
                memcpy(p_row_buffer, addr, size);
                p_row_buffer += size;
                addr += size;
            }
        }

    }
    TupleRow* r = make_tuple(row_buffer);
    r->set_null_values(null_values); // copy the received null vector into new tuple row
    return r;
}

