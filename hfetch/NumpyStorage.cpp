#include "NumpyStorage.h"


NumpyStorage::NumpyStorage(std::string table, std::string keyspace, std::shared_ptr<StorageInterface> storage, ArrayPartitioner &algorithm) {
    this->storage = storage;
    this->partitioner = algorithm;
    std::vector< std::map<std::string,std::string> > keysnames = {
            {{"name", "storage_id"}},{{"name","attr_name"}},
            {{"name", "cluster_id"}},{{"name","block_id"}}
    };
    std::vector< std::map<std::string,std::string> > colsnames = {
            {{"name", "payload"}}
    };

    std::map <std::string, std::string> config;
    config["writer_par"] = "4";
    config["writer_buffer"] = "20";

    this->writer = this->storage->make_writer(table.c_str(),keyspace.c_str(),keysnames,colsnames,config);
}


NumpyStorage::~NumpyStorage() {
    if (writer) delete (writer);
};


const ArrayMetadata* NumpyStorage::store(std::string attr_name, const CassUuid &storage_id, PyArrayObject* numpy) const {
    ArrayMetadata *np_metas = get_np_metadata(numpy);
    void* data = PyArray_BYTES(numpy);
    std::vector<Partition> parts = partitioner.make_partitions(np_metas,data); //z-order or whatever we want

    char *keys, *values, *attr_name_c = nullptr;

    uint32_t offset = 0, keys_size = sizeof(uint64_t*)+sizeof(char*)+sizeof(int32_t)*2;
    uint64_t *c_uuid = nullptr;
    for (Partition part: parts) {
        keys = (char*) malloc(keys_size);
        //UUID
        c_uuid = new uint64_t[2];
        c_uuid[0] = storage_id.time_and_version;
        c_uuid[1] = storage_id.clock_seq_and_node;
        memcpy(keys, &c_uuid, sizeof(uint64_t*));
        offset = sizeof(uint64_t*);

        //ATTR NAME
        attr_name_c = strdup(attr_name.c_str());
        memcpy(keys+offset,&attr_name_c,sizeof(char*));
        offset += sizeof(char *);
        //Cluster id
        memcpy(keys+offset,&part.cluster_id,sizeof(int32_t));
        offset += sizeof(int32_t);
        //Block id
        memcpy(keys+offset,&part.block_id,sizeof(int32_t));
        //COPY VALUES

        values = (char*) malloc (sizeof(char*));
        memcpy(values,&part.data,sizeof(char*));
        //FINALLY WE WRITE THE DATA
        writer->write_to_cassandra(keys,values);
    }
    return np_metas;
}



PyObject* NumpyStorage::read(std::string table, std::string keyspace, std::string attr_name, const CassUuid &storage_id, const ArrayMetadata *arr_meta) {

    std::vector< std::map<std::string,std::string> > keysnames = {
            {{"name", "storage_id"}}, {{"name","attr_name"}},
            {{"name", "cluster_id"}}
    };
    std::vector< std::map<std::string,std::string> > colsnames = {
            {{"name", "block_id"}}, {{"name", "payload"}}
    };

    std::map <std::string, std::string> config;
    config["writer_par"] = "4";
    config["writer_buffer"] = "20";

    CacheTable* cache = this->storage->make_cache(table.c_str(),keyspace.c_str(),keysnames,colsnames,config);
    std::shared_ptr<const std::vector<ColumnMeta> > keys_metas = cache->get_metadata()->get_keys();
    const ColumnMeta last_col_meta = keys_metas->at(keys_metas->size()-1);
    uint32_t keys_size = last_col_meta.size+last_col_meta.position;


    std::vector<const TupleRow*> result, all_results;
    std::vector<Partition> all_partitions;

    uint64_t *c_uuid  = nullptr;
    char *buffer, *key_attr_name = nullptr;
    int32_t cluster_id = 0, offset = 0;
    int32_t *block = nullptr;
    do {
        buffer=(char*) malloc(keys_size);
        //UUID
        c_uuid = new uint64_t[2];
        c_uuid[0] = storage_id.time_and_version;
        c_uuid[1] = storage_id.clock_seq_and_node;
        memcpy(buffer, &c_uuid, sizeof(uint64_t*));
        offset = sizeof(uint64_t*);
        //Attribute name
        key_attr_name = strdup(attr_name.c_str());
        memcpy(buffer+offset, &key_attr_name, sizeof(char *));
        offset += sizeof(char*);
        //Cluster id
        memcpy(buffer+offset,&cluster_id,sizeof(cluster_id));
        //We fetch the data
        result = cache->get_crow(new TupleRow(keys_metas,keys_size,buffer));
        //build cluster
        all_results.insert(all_results.end(),result.begin(),result.end());
        for (const TupleRow* row:result) {
            block = (int32_t *) row->get_element(0);
            all_partitions.push_back(Partition(*block,cluster_id,row->get_element(1)));
        }
        ++cluster_id;
    }
    while (!(result.empty()));


    delete(cache);

    if (all_partitions.empty()) {
        throw ModuleException("no npy found on sys");
    }

    void * data = partitioner.merge_partitions(arr_meta,all_partitions);

    for (const TupleRow* item:result) delete(item);
    npy_intp *dims = new npy_intp[arr_meta->dims.size()];
    for (uint32_t i = 0; i<arr_meta->dims.size(); ++i) {
        dims[i]=arr_meta->dims[i];
    }
    try {
        return PyArray_SimpleNewFromData((int32_t) arr_meta->dims.size(),
                                         dims,
                                arr_meta->inner_type, data);
    }
    catch (std::exception e) {
        if (PyErr_Occurred()) PyErr_Print();
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}


ArrayMetadata* NumpyStorage::get_np_metadata(PyArrayObject *numpy) const {
    int64_t ndims = PyArray_NDIM(numpy);
    npy_intp *shape = PyArray_SHAPE(numpy);

    ArrayMetadata *shape_and_type = new ArrayMetadata();
    shape_and_type->inner_type = PyArray_TYPE(numpy);
    shape_and_type->dims=std::vector<int32_t>((uint64_t)ndims);//PyArray_SHAPE()
    for (int32_t dim = 0; dim<ndims; ++dim){
        shape_and_type->dims[dim]=(int32_t) shape[dim];
    }
    return shape_and_type;
}
