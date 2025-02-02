#ifndef PREFETCHER_CACHE_TABLE_H
#define PREFETCHER_CACHE_TABLE_H

#include <iostream>
#include <cassandra.h>
#include <cstring>
#include <string>
#include <memory>

#include "TimestampGenerator.h"
#include "TupleRow.h"
#include "TupleRowFactory.h"
#include "KVCache.h"
#include "Writer.h"
#include <librdkafka/rdkafka.h>


class CacheTable {

public:
    CacheTable(const TableMetadata *table_meta,
               CassSession *session,
               std::map<std::string, std::string> &config);

    ~CacheTable();

    const TableMetadata *get_metadata() {
        return table_metadata;
    }

    const void flush_elements() const;
    const void wait_elements() const ;

    /*** TupleRow ops ***/

    std::vector<const TupleRow *> get_crow(const TupleRow *py_keys);
    std::vector<const TupleRow *> retrieve_from_cassandra(const TupleRow *keys, const char* attr_name=NULL );

    void put_crow(const TupleRow *keys, const TupleRow *values);
    void send_event(const TupleRow *keys, const TupleRow *values);
    void close_stream();

    void delete_crow(const TupleRow *keys);

    /*** Raw pointers ops ***/

    void put_crow(void *keys, void *values);

    std::vector<const TupleRow *> get_crow(void *keys);
    std::vector<const TupleRow *> retrieve_from_cassandra(void *keys, const char* attr_name=NULL );

    void add_to_cache(void *keys, void *values);
    void add_to_cache(const TupleRow *keys, const TupleRow *values);

    /*** Get access to the writer ***/
    Writer * get_writer();

    /*** Stream operations ***/
    void  enable_stream(const char * topic_name, std::map<std::string, std::string> &config);
    void  enable_stream_producer(void);
    void  enable_stream_consumer(void);
    void poll(char *data, const uint64_t size);
    std::vector<const TupleRow *>  poll(void);

    /*** Auxiliary methods ***/
    const TupleRow* get_new_keys_tuplerow(void* keys) const;
    const TupleRow* get_new_values_tuplerow(void* values) const;

private:
    rd_kafka_message_t * kafka_poll(void) ;


    /* CASSANDRA INFORMATION FOR RETRIEVING DATA */
    CassSession *session;
    const CassPrepared *prepared_query, *delete_query;

    bool disable_timestamps;
    TimestampGenerator *timestamp_gen;

    //Key and Value copy constructed
    KVCache<TupleRow, TupleRow> *myCache;

    TupleRowFactory *keys_factory;
    TupleRowFactory *values_factory;
    TupleRowFactory *row_factory;

    const TableMetadata *table_metadata;

    Writer *writer;
    /*** Stream information ***/
    char * topic_name;
    std::map<std::string, std::string> stream_config;
    rd_kafka_conf_t *kafka_conf;
    rd_kafka_t *consumer;
};

#endif //PREFETCHER_CACHE_TABLE_H
