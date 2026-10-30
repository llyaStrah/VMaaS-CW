#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libpq-fe.h>
#include <librdkafka/rdkafka.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;
namespace {
std::string env(const char* key, const char* fallback) { const char* v=std::getenv(key); return v?v:fallback; }
struct Response { int status=200; std::string body; std::string type="application/json"; };
std::string escape(const std::string& s) { std::string o; for(char c:s){ if(c=='"'||c=='\\')o+='\\'; if(c=='\n')o+="\\n"; else if(c!='\r')o+=c; } return o; }
Response error(int status,const std::string& code,const std::string& msg){return {status,json({{"code",code},{"message",msg}}).dump()};}
PGconn* db=nullptr; rd_kafka_t* producer=nullptr;
PGresult* query(const char* sql,int n=0,const char* const* values=nullptr){ return PQexecParams(db,sql,n,nullptr,values,nullptr,nullptr,0); }
Response handle(const std::string& method,const std::string& path,const std::string& body){
 if(method=="GET"&&path=="/health"){ PGresult* r=PQexec(db,"SELECT 1"); bool ok=r&&PQresultStatus(r)==PGRES_TUPLES_OK; if(r)PQclear(r); return {ok?200:503,json({{"status",ok?"ok":"unavailable"},{"service","vmaas-api"}}).dump()}; }
 if(method=="GET"&&path=="/api/v1/catalog/flavors"){
  PGresult* r=PQexec(db,"SELECT id,name,vcpu,memory_mb,disk_gb FROM flavors WHERE active ORDER BY id"); if(!r||PQresultStatus(r)!=PGRES_TUPLES_OK){if(r)PQclear(r);return error(500,"DB_ERROR","Could not load flavor catalog");}
  json a=json::array(); for(int i=0;i<PQntuples(r);++i)a.push_back({{"id",PQgetvalue(r,i,0)},{"name",PQgetvalue(r,i,1)},{"vcpu",atoi(PQgetvalue(r,i,2))},{"memoryMb",atoi(PQgetvalue(r,i,3))},{"diskGb",atoi(PQgetvalue(r,i,4))}}); PQclear(r); return {200,json({{"items",a}}).dump()};
 }
 if(method=="POST"&&path=="/api/v1/orders"){
  json in; try{in=json::parse(body);}catch(...){return error(400,"INVALID_JSON","Request body must be valid JSON");}
  for(const char* k:{"projectId","imageId","flavorId","vmName","idempotencyKey"}) if(!in.contains(k))return error(400,"VALIDATION_ERROR",std::string("Missing field: ")+k);
  const std::string project=in["projectId"].is_string()?in["projectId"].get<std::string>():std::to_string(in["projectId"].get<long long>());
  const std::string image=in["imageId"].is_string()?in["imageId"].get<std::string>():std::to_string(in["imageId"].get<long long>());
  const std::string flavor=in["flavorId"].is_string()?in["flavorId"].get<std::string>():std::to_string(in["flavorId"].get<long long>());
  std::string name=in["vmName"].get<std::string>(); std::string key=in["idempotencyKey"].get<std::string>();
  if(name.empty()||name.size()>63||key.empty()||key.size()>128)return error(400,"VALIDATION_ERROR","vmName must be 1-63 chars and idempotencyKey 1-128 chars");
  const char* access[]={project.c_str(),image.c_str(),flavor.c_str()};
  PGresult* check=query("SELECT 1 FROM project_members pm JOIN projects p ON p.id=pm.project_id JOIN images i ON i.id=$2 AND i.active JOIN flavors f ON f.id=$3 AND f.active WHERE pm.project_id=$1 AND pm.user_id=1 AND p.status='ACTIVE'",3,access);
  if(!check||PQresultStatus(check)!=PGRES_TUPLES_OK){if(check)PQclear(check);return error(500,"DB_ERROR","Validation query failed");} bool allowed=PQntuples(check)>0; PQclear(check); if(!allowed)return error(422,"ORDER_NOT_ALLOWED","Project, membership, image or flavor is invalid");
  const char* idem[]={project.c_str(),key.c_str()}; PGresult* prior=query("SELECT id,state FROM orders WHERE project_id=$1 AND requested_by=1 AND idempotency_key=$2",2,idem);
  if(prior&&PQresultStatus(prior)==PGRES_TUPLES_OK&&PQntuples(prior)>0){json out={{"orderId",PQgetvalue(prior,0,0)},{"state",PQgetvalue(prior,0,1)},{"idempotentReplay",true}};PQclear(prior);return {200,out.dump()};} if(prior)PQclear(prior);
  if(PQexec(db,"BEGIN")&&PQstatus(db)==CONNECTION_OK){}
  const char* vals[]={project.c_str(),image.c_str(),flavor.c_str(),name.c_str(),key.c_str()};
  PGresult* r=query("INSERT INTO orders(id,project_id,requested_by,image_id,flavor_id,vm_name,state,idempotency_key,correlation_id) VALUES(gen_random_uuid(),$1,1,$2,$3,$4,'PENDING',$5,gen_random_uuid()) RETURNING id,correlation_id",5,vals);
  if(!r||PQresultStatus(r)!=PGRES_TUPLES_OK){if(r)PQclear(r);PQexec(db,"ROLLBACK");return error(409,"ORDER_CREATE_FAILED","Could not create order (possibly duplicate idempotency key)");}
  std::string id=PQgetvalue(r,0,0),corr=PQgetvalue(r,0,1);PQclear(r);
  const char* ev[]={id.c_str()}; PGresult* e=query("INSERT INTO order_events(order_id,to_state,event_type) VALUES($1,'PENDING','ORDER_CREATED')",1,ev);if(e)PQclear(e);
  json payload={{"eventId",id},{"eventType","vm.provision.requested"},{"schemaVersion",1},{"orderId",id},{"correlationId",corr}};
  const std::string p=payload.dump(); rd_kafka_resp_err_t kerr=rd_kafka_producev(producer,RD_KAFKA_V_TOPIC("vm.provision.requested"),RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),RD_KAFKA_V_VALUE(const_cast<char*>(p.data()),p.size()),RD_KAFKA_V_KEY(const_cast<char*>(id.data()),id.size()),RD_KAFKA_V_END);
  bool queued=(kerr==RD_KAFKA_RESP_ERR_NO_ERROR); if(queued)rd_kafka_poll(producer,0);
  PGresult* commit=PQexec(db,"COMMIT"); bool committed=commit&&PQresultStatus(commit)==PGRES_COMMAND_OK;if(commit)PQclear(commit);
  if(!committed)return error(500,"DB_COMMIT_FAILED","Order transaction failed");
  return {202,json({{"orderId",id},{"correlationId",corr},{"state","PENDING"},{"kafkaQueued",queued}}).dump()};
 }
 if(method=="GET"&&path.rfind("/api/v1/orders/",0)==0){std::string id=path.substr(15); const char* v[]={id.c_str()}; PGresult* r=query("SELECT id,state,vm_name,created_at::text FROM orders WHERE id=$1",1,v); if(!r||PQresultStatus(r)!=PGRES_TUPLES_OK){if(r)PQclear(r);return error(500,"DB_ERROR","Could not load order");} if(!PQntuples(r)){PQclear(r);return error(404,"NOT_FOUND","Order not found");} json out={{"orderId",PQgetvalue(r,0,0)},{"state",PQgetvalue(r,0,1)},{"vmName",PQgetvalue(r,0,2)},{"createdAt",PQgetvalue(r,0,3)}};PQclear(r);return {200,out.dump()}; }
 return error(404,"NOT_FOUND","Route not found");
}
void serve(int fd){
 std::string raw; char buf[4096]; size_t expected=0,headerEnd=std::string::npos;
 while(raw.size()<1024*1024){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<=0)return;raw.append(buf,n);headerEnd=raw.find("\r\n\r\n");if(headerEnd!=std::string::npos){auto h=raw.substr(0,headerEnd);auto pos=h.find("Content-Length:");if(pos!=std::string::npos)expected=static_cast<size_t>(std::stoul(h.substr(pos+15)));if(raw.size()>=headerEnd+4+expected)break;}}
 if(headerEnd==std::string::npos)return;std::istringstream hs(raw.substr(0,raw.find("\r\n")));std::string method,path,ver;hs>>method>>path>>ver;
 try{while(raw.size()<headerEnd+4+expected){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<=0)break;raw.append(buf,n);}}catch(...){}
 std::string body=raw.substr(headerEnd+4,expected);Response res;try{res=handle(method,path,body);}catch(const std::exception&){res=error(500,"INTERNAL_ERROR","Unexpected server error");}
 const char* reason=res.status==200?"OK":res.status==202?"Accepted":res.status==400?"Bad Request":res.status==404?"Not Found":res.status==422?"Unprocessable Entity":res.status==409?"Conflict":res.status==503?"Service Unavailable":"Internal Server Error";
 std::ostringstream out;out<<"HTTP/1.1 "<<res.status<<" "<<reason<<"\r\nContent-Type: "<<res.type<<"\r\nContent-Length: "<<res.body.size()<<"\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Idempotency-Key\r\nConnection: close\r\n\r\n"<<res.body;std::string response=out.str();send(fd,response.data(),response.size(),0);
}
}
int main(){
 const std::string dburl=env("DATABASE_URL","postgresql://vmaas:vmaas_dev_password@localhost:5432/vmaas");
 for(int attempt=0;attempt<40;++attempt){db=PQconnectdb(dburl.c_str());if(PQstatus(db)==CONNECTION_OK)break;PQfinish(db);db=nullptr;std::this_thread::sleep_for(std::chrono::seconds(2));}
 if(!db){std::cerr<<"database connection failed\n";return 1;}
 char err[512];rd_kafka_conf_t* conf=rd_kafka_conf_new();rd_kafka_conf_set(conf,"bootstrap.servers",env("KAFKA_BOOTSTRAP_SERVERS","localhost:9092").c_str(),err,sizeof(err));producer=rd_kafka_new(RD_KAFKA_PRODUCER,conf,err,sizeof(err));if(!producer){std::cerr<<"Kafka init failed: "<<err<<"\n";return 1;}
 int port=std::atoi(env("PORT","8080").c_str());int server=socket(AF_INET,SOCK_STREAM,0);int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons(port);if(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0||listen(server,64)<0){perror("listen");return 1;}std::cout<<"VMaaS API listening on "<<port<<std::endl;
 while(true){int fd=accept(server,nullptr,nullptr);if(fd<0)continue;std::thread(serve,fd).detach();}
}
