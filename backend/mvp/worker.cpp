#include <libpq-fe.h>
#include <librdkafka/rdkafka.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
using json=nlohmann::json;
std::string env(const char* k,const char* d){const char* v=std::getenv(k);return v?v:d;}
int main(){
 auto dburl=env("DATABASE_URL","postgresql://vmaas:vmaas_dev_password@localhost:5432/vmaas");auto brokers=env("KAFKA_BOOTSTRAP_SERVERS","localhost:9092");
 rd_kafka_conf_t* c=rd_kafka_conf_new();char err[512];rd_kafka_conf_set(c,"bootstrap.servers",brokers.c_str(),err,sizeof(err));rd_kafka_conf_set(c,"group.id","vmaas-provisioning-mvp",err,sizeof(err));rd_kafka_conf_set(c,"auto.offset.reset","earliest",err,sizeof(err));rd_kafka_conf_set(c,"enable.auto.commit","false",err,sizeof(err));rd_kafka_t* consumer=rd_kafka_new(RD_KAFKA_CONSUMER,c,err,sizeof(err));if(!consumer){std::cerr<<err<<"\n";return 1;}rd_kafka_poll_set_consumer(consumer);rd_kafka_topic_partition_list_t* topics=rd_kafka_topic_partition_list_new(1);rd_kafka_topic_partition_list_add(topics,"vm.provision.requested",RD_KAFKA_PARTITION_UA);if(rd_kafka_subscribe(consumer,topics)!=RD_KAFKA_RESP_ERR_NO_ERROR){std::cerr<<"subscribe failed\n";return 1;}rd_kafka_topic_partition_list_destroy(topics);
 for(;;){rd_kafka_message_t* m=rd_kafka_consumer_poll(consumer,1000);if(!m)continue;if(m->err){rd_kafka_message_destroy(m);continue;}
  try{auto ev=json::parse(std::string(static_cast<char*>(m->payload),m->len));std::string oid=ev.at("orderId").get<std::string>();PGconn* db=PQconnectdb(dburl.c_str());if(PQstatus(db)==CONNECTION_OK){const char* vals[]={oid.c_str()};PGresult* r=PQexecParams(db,"UPDATE orders SET state='PROVISIONING',updated_at=now() WHERE id=$1 AND state='PENDING'",1,nullptr,vals,nullptr,nullptr,0);if(r)PQclear(r);std::this_thread::sleep_for(std::chrono::seconds(2));std::string provider="mock-"+oid;const char* vmvals[]={oid.c_str(),provider.c_str()};r=PQexecParams(db,"WITH changed AS (UPDATE orders SET state='READY',updated_at=now() WHERE id=$1 AND state='PROVISIONING' RETURNING id,project_id,vm_name) INSERT INTO virtual_machines(order_id,project_id,provider_vm_id,name,state) SELECT id,project_id,$2,vm_name,'ACTIVE' FROM changed ON CONFLICT(order_id) DO NOTHING",2,nullptr,vmvals,nullptr,nullptr,0);if(r)PQclear(r);r=PQexecParams(db,"INSERT INTO order_events(order_id,from_state,to_state,event_type) VALUES($1,'PROVISIONING','READY','MOCK_PROVISIONED') ON CONFLICT DO NOTHING",1,nullptr,vals,nullptr,nullptr,0);if(r)PQclear(r);PQfinish(db);std::cout<<"mock provisioned order "<<oid<<std::endl;}rd_kafka_commit_message(consumer,m,0);
  }catch(const std::exception& e){std::cerr<<"bad event: "<<e.what()<<"\n";rd_kafka_commit_message(consumer,m,0);}rd_kafka_message_destroy(m);
 }
}
