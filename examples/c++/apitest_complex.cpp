#include <hecuba/HecubaSession.h>
#include <hecuba/IStorage.h>
#include <iostream>

#define ROWS 4
#define COLS 3
char * generateKey(float lat, int ts) {

    char * key = (char*) malloc (sizeof(float) + sizeof(int));
    float *lat_key = (float*) key;
    *lat_key = lat;
    int *ts_key = (int*) (key + sizeof(float));
    *ts_key = ts;
    return key;
}

char * generateMetas() {
    unsigned int * metas = (unsigned int *) malloc(sizeof(unsigned int) * 3);

    metas[0]=2; // number of dimmensions
    metas[1]=ROWS; // number of elements in the first dimmension
    metas[2]=COLS; // number of elements in the second dimmension
    //metas[3]=sizeof(double); //'f' ONLY FLOATS SUPPORTED

    return (char *) metas;
}

char * generateNumpyContent() {

    double *numpy=(double*)malloc(sizeof(double)*COLS*ROWS);
    double *tmp = numpy;
    double num = 1;
    for (int i=0; i<ROWS; i++) {
        for (int j=0; j<COLS; j++) {
            std::cout<< "++ "<<i<<","<<j<<std::endl;
            *tmp = num++;
            tmp+=1;
        }
    }
    return (char*) numpy;
}

int main() {
    std::cout<< "+ STARTING C++ APP"<<std::endl;
    HecubaSession s;
    std::cout<< "+ Session started"<<std::endl;

    s.loadDataModel("model_complex.yaml","model_complex.py");
    std::cout<< "+ Data Model loaded"<<std::endl;

    // create a new StorageObject: "myclass" is the name of the class defined in model_complex.yaml and "mysim" is the name of the persistent object 

    IStorage* myobj = s.createObject("myclass", "mysim");

    ///////////////////////////////////////////// 

    // set values to each attribute: name of the attribute (as specified in model_storageobj.yaml) followed by a pointer to the value

    ///////////////////////////////////////////// 
    // Attribute "sim_id" of type text
    char *sim_id = (char *) malloc(3);
    strcpy(sim_id,"id");
    myobj->setAttr("sim_id", &sim_id);

    ///////////////////////////////////////////// 
    // Attribute "sim_info" of type info which is a StorageObject. We first create the StorageObject myobj2 and then we can set the attribute of myobj
    IStorage* myobj2 = s.createObject("info", "sim_description");
    int value = 10000;
    myobj2->setAttr("total_ts", &value);
    value=10;
    myobj2->setAttr("output_freq", &value);

    myobj->setAttr("sim_info", myobj2);

    ///////////////////////////////////////////// 
    // Attribute "submetrics  of type metrics which is a StorageDict. We first create the StorageDict mydict and then we can set the attribute of myobj

    IStorage* mydict = s.createObject("metrics", "outputDict");

	float lat = 0.666;
    int ts = 42;
    char *key;
    key=generateKey(lat,ts);
    char * numpymeta;
    numpymeta = generateMetas();
    char *valueNP = generateNumpyContent();
    IStorage *mi_sn=s.createObject("hecuba.hnumpy.StorageNumpy","minp",numpymeta,valueNP);


    mydict->setItem(key, mi_sn);

    myobj->setAttr("submetrics",mydict);

    std::cout<< "+ completed: syncing"<<std::endl;

    // we sync every thing before ending the process
#if 0
    mi_sn->sync();
    mydict->sync();
    myobj->sync();
#endif
}
