#include "Prefetch.h"

#define MAX_TRIES 10
#define default_prefetch_size 100

Prefetch::Prefetch(const std::vector<std::pair<int64_t, int64_t>> &token_ranges, const TableMetadata *table_meta,
                   CassSession *session, std::map<std::string, std::string> &config) {
    if (!session)
        throw ModuleException("Prefetch: Session is Null, not connected to Cassandra");
    this->session = session;
    this->table_metadata = table_meta;

    const char *query;
    if (config["type"] == "keys") {
        this->t_factory = TupleRowFactory(table_meta->get_keys());
        query = table_meta->get_select_keys_tokens();
        this->type = "keys";
    } else if (config["type"] == "values") {
        this->t_factory = TupleRowFactory(table_meta->get_values());
        query = table_meta->get_select_values_tokens();
        this->type = "values";
    } else {
        this->t_factory = TupleRowFactory(table_meta->get_items());
        query = table_meta->get_select_all_tokens();
        this->type = "items";
    }

    if (config.find("custom_select") != config.end()) {
        char connect[6] = " AND ";
        std::string restriction = config["custom_select"];

        char *new_query = (char *) malloc(std::strlen(query) + restriction.length() + 5);
        char *first_elem = new_query;

        // Copy original query

        memcpy(new_query, query, std::strlen(query) - 1);
        new_query += std::strlen(query) - 1;

        // Add AND connector
        memcpy(new_query, &connect, sizeof(char) * 5);
        new_query += 5;

        // Copy user restriction
        memcpy(new_query, restriction.c_str(), restriction.length() + 1);
        query = first_elem;
    }

    this->tokens = token_ranges;
    this->completed = false;
    CassFuture *future = cass_session_prepare(session, query);
    CassError rc = cass_future_error_code(future);
    CHECK_CASS("prefetch cannot prepare query:" + std::string(query));
    this->prepared_query = cass_future_get_prepared(future);
    cass_future_free(future);

    int32_t prefetch_size = default_prefetch_size;

    if (config.find("prefetch_size") != config.end()) {
        std::string prefetch_size_str = config["prefetch_size"];
        try {
            prefetch_size = std::stoi(prefetch_size_str);
        }
        catch (std::exception &e) {
            std::string msg(e.what());
            msg += " Malformed value in config for prefetch_size";
            throw ModuleException(msg);
        }
    }

    if (prefetch_size <= 0)
        throw ModuleException("Prefetch size must be > 0");

    this->data.set_capacity(prefetch_size);
    this->worker = new std::thread{&Prefetch::consume_tokens, this};

}

Prefetch::~Prefetch() {
    data.set_capacity(0);

    while (!completed) data.abort();

    worker->join();
    delete (worker);

    TupleRow *to_delete;
    while (data.try_pop(to_delete)) delete (to_delete);

    if (this->prepared_query != NULL) cass_prepared_free(this->prepared_query);
}


TupleRow *Prefetch::get_cnext() {
    if (completed && data.empty()) return NULL;
    TupleRow *response;
    try {
        data.pop(response);
    }
    catch (std::exception &e) {
        if (data.empty()) return NULL;
        else return get_cnext();
    }
    return response;
}


void Prefetch::consume_tokens() {
    for (std::pair<int64_t, int64_t> &range : tokens) {
        //If Consumer sets capacity 0, we stop fetching data
        if (data.capacity() == 0) {
            completed = true;
            data.abort();
            return;
        }

        //Bind tokens and execute
        CassStatement *statement = cass_prepared_bind(this->prepared_query);
        cass_statement_bind_int64(statement, 0, range.first);
        cass_statement_bind_int64(statement, 1, range.second);
        CassFuture *future = cass_session_execute(session, statement);
        cass_statement_free(statement);

        const CassResult *result = NULL;
        int tries = 0;

        while (result == NULL) {
            //If Consumer sets capacity 0, we stop fetching data
            if (data.capacity() == 0) {
                cass_future_free(future);
                completed = true;
                data.abort();
                return;
            }

            result = cass_future_get_result(future);
            CassError rc = cass_future_error_code(future);

            if (rc != CASS_OK) {
                std::cerr << "Prefetch action failed: " << cass_error_desc(rc) << " Try #" << tries << std::endl;
                tries++;
                if (tries > MAX_TRIES) {
                    cass_future_free(future);
                    completed = true;
                    data.abort();
                    std::cerr << "Prefetch reached max connection attempts " << MAX_TRIES << std::endl;
                    std::cerr << "Prefetch query " << this->prepared_query << std::endl;
                    return;
                }
            }
        }


        //PRE: Result != NULL, future != NULL, completed = false
        cass_future_free(future);

        CassIterator *iterator = cass_iterator_from_result(result);
        while (cass_iterator_next(iterator)) {
            if (data.capacity() == 0) {
                completed = true;
                data.abort();
                cass_iterator_free(iterator);
                cass_result_free(result);
                return;
            }
            const CassRow *row = cass_iterator_get_row(iterator);
            TupleRow *t = t_factory.make_tuple(row);
            try {
                data.push(t); //blocking operation
            }
            catch (std::exception &e) {
                completed = true;
                data.abort();
                delete (t);
                cass_iterator_free(iterator);
                cass_result_free(result);
                return;
            }
        }
        //Done fetching current token range
        cass_iterator_free(iterator);
        cass_result_free(result);
    }
    //All token ranges fetched
    completed = true;
    data.abort();
}