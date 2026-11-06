/* C11 Win32 / Winsock overlapped I/O + I/O completion port teaching demo.
   Control-plane accept/connect may block; every data-plane read/write uses
   WSARecv/WSASend with its own OVERLAPPED. No completion routines are used. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

#define MAX_PEERS 8
#define MAX_POOL 8
#define HEADER 24u
#define MAX_PAYLOAD (4u*1024u*1024u)
#define MAGIC 0x534f4333u
enum { HELLO=1, TASK=2, RESULT=3, START=4, DATA=5, END_DATA=6, ACK=7, DONE=8, QUIT=9, BYE=10 };
typedef struct Peer Peer;
typedef struct Message { struct Message *next; unsigned char *bytes; DWORD size, sent, type; } Message;
typedef struct Operation { OVERLAPPED ov; Peer *peer; int writing; } Operation;
typedef struct Config {
 int server, bench, up, workers, id, threads, active, port, delay_ms, io_slice, timeout;
 uint32_t elements, chunk; uint64_t bytes;
 char host[64];
} Config;
struct Peer {
 SOCKET socket; CRITICAL_SECTION lock; int index, id, hello, closing, result, bye, quitting;
 Operation receive, send;
 unsigned char header[HEADER], *payload;
 DWORD rx_have, rx_target; int rx_body;
 uint32_t type, job, sequence, length, aux;
 Message *first, *last; int sending;
 uint64_t tx_bytes, rx_bytes, errors, sum, expected;
 uint32_t tx_seq, rx_seq, seed, task_offset, task_count;
 int bulk_sender, receiving_bulk, saw_end;
 double elapsed;
};
static Config cfg;
static Peer peers[MAX_PEERS];
static HANDLE port_handle, complete_event, ready_event, bye_event, threads[MAX_POOL];
static volatile LONG outstanding, stopping, failed, hello_count, result_count, bye_count;
static LARGE_INTEGER frequency, benchmark_start;
static CRITICAL_SECTION print_lock;
static uint64_t completions[MAX_POOL];
static DWORD pool_ids[MAX_POOL];
static int peer_count;
static uint64_t merged_sum, reference_sum;

static void logline(const char *fmt, ...) {
 va_list ap; EnterCriticalSection(&print_lock); va_start(ap,fmt); vprintf(fmt,ap); va_end(ap);
 putchar('\n'); fflush(stdout); LeaveCriticalSection(&print_lock);
}
static void fail(const char *where, unsigned long error) {
 if (InterlockedCompareExchange(&failed,1,0)==0) logline("ERROR where=%s code=%lu",where,error);
 SetEvent(complete_event); SetEvent(ready_event); SetEvent(bye_event);
}
static void put32(unsigned char *p,uint32_t x) { uint32_t v=htonl(x); memcpy(p,&v,4); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return ntohl(v); }
static void put64(unsigned char *p,uint64_t x) { put32(p,(uint32_t)(x>>32)); put32(p+4,(uint32_t)x); }
static uint64_t get64(const unsigned char *p) { return ((uint64_t)get32(p)<<32)|get32(p+4); }
static double since_start(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)(t.QuadPart-benchmark_start.QuadPart)/(double)frequency.QuadPart; }
static uint32_t value_at(uint32_t i) { return (uint32_t)(((uint64_t)i*17u+23u)%1009u); }
static int selected(Peer *p) { return cfg.active==0 || p->id==cfg.active; }
static int active_count(void) { return cfg.active==0?cfg.workers:1; }
static void post_send(Peer *p);
static void post_receive(Peer *p);
static void pump_bulk(Peer *p);

static void queue_frame(Peer *p,uint32_t type,uint32_t job,uint32_t seq,uint32_t aux,const unsigned char *payload,DWORD length) {
 Message *m;
 if (InterlockedCompareExchange(&stopping,0,0) || InterlockedCompareExchange(&failed,0,0)) return;
 if (length>MAX_PAYLOAD) { fail("outgoing length",length); return; }
 m=(Message*)calloc(1,sizeof(*m)); if(!m) {fail("calloc message",ERROR_OUTOFMEMORY);return;}
 m->size=HEADER+length; m->type=type; m->bytes=(unsigned char*)malloc(m->size);
 if(!m->bytes){free(m);fail("malloc frame",ERROR_OUTOFMEMORY);return;}
 put32(m->bytes,MAGIC);put32(m->bytes+4,type);put32(m->bytes+8,job);
 put32(m->bytes+12,seq);put32(m->bytes+16,length);put32(m->bytes+20,aux);
 if(length)memcpy(m->bytes+HEADER,payload,length);
 if(p->last)p->last->next=m;else p->first=m;p->last=m;
 if(!p->sending)post_send(p);
}
static void post_send(Peer *p) {
 WSABUF b; DWORD n; int r,e;
 Message *m=p->first;
 if(!m || p->closing || InterlockedCompareExchange(&stopping,0,0) || InterlockedCompareExchange(&failed,0,0))return;
 n=m->size-m->sent;if(cfg.io_slice && n>(DWORD)cfg.io_slice)n=(DWORD)cfg.io_slice;
 b.buf=(char*)m->bytes+m->sent;b.len=n;
 ZeroMemory(&p->send.ov,sizeof(OVERLAPPED));p->sending=1;
 InterlockedIncrement(&outstanding);
 r=WSASend(p->socket,&b,1,NULL,0,&p->send.ov,NULL);
 if(r==SOCKET_ERROR && (e=WSAGetLastError())!=WSA_IO_PENDING){p->sending=0;InterlockedDecrement(&outstanding);fail("WSASend",(unsigned long)e);}
}
static void post_receive(Peer *p) {
 WSABUF b; DWORD flags=0,n; int r,e;
 if(p->closing || p->quitting || InterlockedCompareExchange(&stopping,0,0) || InterlockedCompareExchange(&failed,0,0))return;
 n=p->rx_target-p->rx_have;if(cfg.io_slice && n>(DWORD)cfg.io_slice)n=(DWORD)cfg.io_slice;
 b.buf=(char*)(p->rx_body?p->payload:p->header)+p->rx_have;b.len=n;
 ZeroMemory(&p->receive.ov,sizeof(OVERLAPPED));
 InterlockedIncrement(&outstanding);
 r=WSARecv(p->socket,&b,1,NULL,&flags,&p->receive.ov,NULL);
 if(r==SOCKET_ERROR && (e=WSAGetLastError())!=WSA_IO_PENDING){InterlockedDecrement(&outstanding);fail("WSARecv",(unsigned long)e);}
}
static uint32_t pattern_word(uint64_t byte_offset,uint32_t seed) {
 return (uint32_t)((byte_offset/4u)*2654435761u)^seed;
}
static void fill_pattern(unsigned char *data,DWORD length,uint64_t offset,uint32_t seed) {
 DWORD i;for(i=0;i<length;i+=4)put32(data+i,pattern_word(offset+i,seed));
}
static uint64_t check_pattern(const unsigned char *data,DWORD length,uint64_t offset,uint32_t seed) {
 DWORD i;uint64_t errors=0;for(i=0;i<length;i+=4)if(get32(data+i)!=pattern_word(offset+i,seed))++errors;return errors;
}
static void pump_bulk(Peer *p) {
 DWORD n;unsigned char *body;
 if(p->tx_bytes>=cfg.bytes) {
  p->bulk_sender=0;queue_frame(p,END_DATA,p->id,p->tx_seq,p->id,NULL,0);return;
 }
 n=(DWORD)((cfg.bytes-p->tx_bytes)<cfg.chunk?(cfg.bytes-p->tx_bytes):cfg.chunk);
 body=(unsigned char*)malloc(n);if(!body){fail("bulk buffer",ERROR_OUTOFMEMORY);return;}
 fill_pattern(body,n,p->tx_bytes,p->seed);
 queue_frame(p,DATA,p->id,p->tx_seq,p->id,body,n);
 p->tx_bytes+=n;++p->tx_seq;free(body);
}
static void result_done(Peer *p) {
 if(p->result){fail("duplicate result",p->id);return;}
 p->result=1;p->elapsed=since_start();
 if(InterlockedIncrement(&result_count)==(cfg.bench?active_count():cfg.workers))SetEvent(complete_event);
}
static void handle_frame(Peer *p) {
 unsigned char out[32];uint32_t i,count;uint64_t sum,expected,received,mismatches;
 if(cfg.server) {
  if(p->type==HELLO) {
   if(p->hello || p->length!=0 || p->aux<1 || p->aux>(uint32_t)cfg.workers){fail("invalid HELLO",p->aux);return;}
   p->id=(int)p->aux;p->hello=1;
   logline("CONNECTED worker=%d peer=%d server_pid=%lu",p->id,p->index,GetCurrentProcessId());
   if(InterlockedIncrement(&hello_count)==cfg.workers)SetEvent(ready_event);
  } else if(p->type==RESULT && !cfg.bench) {
   if(!p->hello || p->job!=(uint32_t)p->id || p->length!=8){fail("invalid task result",p->type);return;}
   p->sum=get64(p->payload);
   if(p->sum!=p->expected){fail("partial sum mismatch",p->id);return;}
   logline("TASK_RESULT worker=%d offset=%u count=%u partial=%llu expected=%llu check=PASS iocp_tid=%lu",p->id,p->task_offset,p->task_count,(unsigned long long)p->sum,(unsigned long long)p->expected,GetCurrentThreadId());
   result_done(p);
  } else if(p->type==DATA && cfg.bench && cfg.up) {
   if(!p->receiving_bulk || !selected(p) || p->job!=(uint32_t)p->id || p->sequence!=p->rx_seq || !p->length || p->length>cfg.chunk || p->length%4 || p->rx_bytes+p->length>cfg.bytes){fail("upload DATA order/length",p->id);return;}
   p->errors+=check_pattern(p->payload,p->length,p->rx_bytes,p->seed);p->rx_bytes+=p->length;++p->rx_seq;
  } else if(p->type==END_DATA && cfg.bench && cfg.up) {
   if(!p->receiving_bulk || p->saw_end || p->rx_bytes!=cfg.bytes || p->errors || p->job!=(uint32_t)p->id || p->sequence!=p->rx_seq || p->length){fail("upload END validation",p->id);return;}
   p->saw_end=1;put64(out,p->rx_bytes);put64(out+8,p->errors);
   queue_frame(p,ACK,p->id,p->rx_seq,p->id,out,16);
  } else if(p->type==DONE && cfg.bench) {
   if(!selected(p) || p->job!=(uint32_t)p->id || p->length!=16){fail("invalid DONE",p->id);return;}
   received=get64(p->payload);mismatches=get64(p->payload+8);
   if(received!=cfg.bytes || mismatches || (cfg.up&&!p->saw_end)){fail("benchmark acknowledgement",p->id);return;}
   if(!cfg.up && p->tx_bytes!=cfg.bytes){fail("download byte accounting",p->id);return;}
   logline("BENCH_DONE worker=%d direction=%s received=%llu pattern_errors=%llu iocp_tid=%lu",p->id,cfg.up?"up":"down",(unsigned long long)received,(unsigned long long)mismatches,GetCurrentThreadId());
   result_done(p);
  } else if(p->type==BYE && p->length==0) {
   if(p->bye){fail("duplicate BYE",p->id);return;}
   p->bye=1;p->closing=1;
   if(InterlockedIncrement(&bye_count)==cfg.workers)SetEvent(bye_event);
  } else fail("unexpected server frame",p->type);
 } else {
  if(p->type==TASK) {
   if(p->length<16 || p->job!=(uint32_t)cfg.id){fail("invalid TASK",p->length);return;}
   count=get32(p->payload+8);
   if(count>1000003u || p->length!=16u+count*4u){fail("TASK body length",count);return;}
   sum=0;for(i=0;i<count;++i)sum+=get32(p->payload+16u+i*4u);
   if(cfg.delay_ms)Sleep((DWORD)cfg.delay_ms);
   put64(out,sum);queue_frame(p,RESULT,p->job,0,cfg.id,out,8);
   logline("TASK_COMPUTED worker=%d offset=%llu count=%u sum=%llu iocp_tid=%lu",cfg.id,(unsigned long long)get64(p->payload),count,(unsigned long long)sum,GetCurrentThreadId());
  } else if(p->type==START) {
   if(p->length!=20 || p->job!=(uint32_t)cfg.id){fail("invalid START",p->length);return;}
   cfg.up=(int)get32(p->payload);cfg.bytes=get64(p->payload+4);cfg.chunk=get32(p->payload+12);p->seed=get32(p->payload+16);
   if(cfg.up>1 || cfg.chunk<4 || cfg.chunk>262144 || cfg.chunk%4 || !cfg.bytes || cfg.bytes%4 || cfg.bytes>(4ull*1024ull*1024ull*1024ull)){fail("START bounds",cfg.chunk);return;}
   p->tx_bytes=p->rx_bytes=p->errors=0;p->tx_seq=p->rx_seq=0;
   logline("BENCH_START worker=%d direction=%s payload=%llu chunk=%u",cfg.id,cfg.up?"up":"down",(unsigned long long)cfg.bytes,cfg.chunk);
   if(cfg.up){p->bulk_sender=1;pump_bulk(p);}else p->receiving_bulk=1;
  } else if(p->type==DATA && !cfg.up) {
   if(!p->receiving_bulk || p->job!=(uint32_t)cfg.id || p->sequence!=p->rx_seq || !p->length || p->length>cfg.chunk || p->length%4 || p->rx_bytes+p->length>cfg.bytes){fail("download DATA order/length",cfg.id);return;}
   p->errors+=check_pattern(p->payload,p->length,p->rx_bytes,p->seed);p->rx_bytes+=p->length;++p->rx_seq;
  } else if(p->type==END_DATA && !cfg.up) {
   if(!p->receiving_bulk || p->saw_end || p->rx_bytes!=cfg.bytes || p->errors || p->job!=(uint32_t)cfg.id || p->sequence!=p->rx_seq || p->length){fail("download END validation",cfg.id);return;}
   p->saw_end=1;put64(out,p->rx_bytes);put64(out+8,p->errors);queue_frame(p,DONE,cfg.id,p->rx_seq,cfg.id,out,16);
   logline("RECEIVE_VERIFIED worker=%d bytes=%llu pattern_errors=%llu",cfg.id,(unsigned long long)p->rx_bytes,(unsigned long long)p->errors);
  } else if(p->type==ACK && cfg.up) {
   if(p->length!=16 || get64(p->payload)!=cfg.bytes || get64(p->payload+8)){fail("upload ACK",cfg.id);return;}
   queue_frame(p,DONE,cfg.id,p->tx_seq,cfg.id,p->payload,16);
   logline("REMOTE_RECEIVE_VERIFIED worker=%d bytes=%llu",cfg.id,(unsigned long long)get64(p->payload));
  } else if(p->type==QUIT && p->length==0) {
   /* Do not post another read after QUIT. The remote may close once BYE is
      received, before our BYE send completion has been dequeued locally. */
   p->quitting=1;
   queue_frame(p,BYE,0,0,cfg.id,NULL,0);
  } else fail("unexpected worker frame",p->type);
 }
 (void)expected;
}
static void received(Peer *p,DWORD bytes) {
 p->rx_have+=bytes;
 if(p->rx_have>p->rx_target){fail("receive overflow",bytes);return;}
 if(p->rx_have<p->rx_target){post_receive(p);return;}
 if(!p->rx_body) {
  if(get32(p->header)!=MAGIC){fail("frame magic",get32(p->header));return;}
  p->type=get32(p->header+4);p->job=get32(p->header+8);p->sequence=get32(p->header+12);
  p->length=get32(p->header+16);p->aux=get32(p->header+20);
  if(p->length>MAX_PAYLOAD){fail("frame length bound",p->length);return;}
  if(p->length){p->rx_body=1;p->rx_target=p->length;p->rx_have=0;post_receive(p);return;}
 }
 handle_frame(p);
 p->rx_body=0;p->rx_have=0;p->rx_target=HEADER;
 post_receive(p);
}
static void sent(Peer *p,DWORD bytes) {
 Message *m=p->first;DWORD kind;
 if(!m || !bytes || bytes>m->size-m->sent){fail("send completion size",bytes);return;}
 m->sent+=bytes;p->sending=0;
 if(m->sent<m->size){post_send(p);return;}
 kind=m->type;p->first=m->next;if(!p->first)p->last=NULL;free(m->bytes);free(m);
 if(kind==BYE && !cfg.server){p->closing=1;SetEvent(complete_event);return;}
 if(p->first)post_send(p);
 else if(p->bulk_sender && (kind==DATA || kind==START))pump_bulk(p);
}
static DWORD WINAPI pool_thread(LPVOID arg) {
 int index=(int)(INT_PTR)arg;DWORD bytes,error;ULONG_PTR key;OVERLAPPED *ov;BOOL ok;Operation *op;Peer *p;
 pool_ids[index]=GetCurrentThreadId();
 logline("IOCP_THREAD index=%d tid=%lu pid=%lu",index,pool_ids[index],GetCurrentProcessId());
 for(;;) {
  ov=NULL;bytes=0;key=0;ok=GetQueuedCompletionStatus(port_handle,&bytes,&key,&ov,INFINITE);
  error=ok?0:GetLastError();if(!ov)break;
  op=(Operation*)ov;p=op->peer;++completions[index];
  EnterCriticalSection(&p->lock);
  if(!InterlockedCompareExchange(&stopping,0,0)) {
   if(!ok){if(!p->closing)fail("failed completion",error);}
   else if(!bytes){if(!p->closing)fail("unexpected peer disconnect",ERROR_CONNECTION_ABORTED);}
   else if(op->writing)sent(p,bytes);else received(p,bytes);
  }
  LeaveCriticalSection(&p->lock);
  InterlockedDecrement(&outstanding);
 }
 return 0;
}
static int wait_event(HANDLE event,const char *where) {
 DWORD r=WaitForSingleObject(event,(DWORD)cfg.timeout*1000u);
 if(r!=WAIT_OBJECT_0){fail(where,r==WAIT_TIMEOUT?WAIT_TIMEOUT:GetLastError());return 0;}
 return !InterlockedCompareExchange(&failed,0,0);
}
static int attach(Peer *p,SOCKET s,int index) {
 ZeroMemory(p,sizeof(*p));p->socket=s;p->index=index;p->rx_target=HEADER;
 p->payload=(unsigned char*)malloc(MAX_PAYLOAD);if(!p->payload){fail("receive allocation",ERROR_OUTOFMEMORY);return 0;}
 InitializeCriticalSection(&p->lock);p->receive.peer=p;p->send.peer=p;p->send.writing=1;
 if(!CreateIoCompletionPort((HANDLE)s,port_handle,(ULONG_PTR)p,0)){fail("associate socket",GetLastError());free(p->payload);DeleteCriticalSection(&p->lock);return 0;}
 EnterCriticalSection(&p->lock);post_receive(p);LeaveCriticalSection(&p->lock);return 1;
}
static void clean(void) {
 int i;Message *m,*next;DWORD start=GetTickCount();
 InterlockedExchange(&stopping,1);
 for(i=0;i<peer_count;++i){EnterCriticalSection(&peers[i].lock);if(peers[i].socket!=INVALID_SOCKET){closesocket(peers[i].socket);peers[i].socket=INVALID_SOCKET;}LeaveCriticalSection(&peers[i].lock);}
 /* closesocket cancels outstanding I/O; retain contexts and buffers while the
    completion packets are drained. Only free after all pool threads join. */
 while(InterlockedCompareExchange(&outstanding,0,0)>0 && GetTickCount()-start<10000u)Sleep(1);
 if(InterlockedCompareExchange(&outstanding,0,0)>0){logline("ERROR completion drain timeout; process exits without freeing pending buffers");ExitProcess(2);}
 for(i=0;i<cfg.threads;++i)PostQueuedCompletionStatus(port_handle,0,0,NULL);
 for(i=0;i<cfg.threads;++i)if(threads[i]){WaitForSingleObject(threads[i],INFINITE);CloseHandle(threads[i]);}
 for(i=0;i<peer_count;++i){for(m=peers[i].first;m;m=next){next=m->next;free(m->bytes);free(m);}free(peers[i].payload);DeleteCriticalSection(&peers[i].lock);}
 for(i=0;i<cfg.threads;++i)logline("IOCP_SUMMARY index=%d tid=%lu completions=%llu",i,pool_ids[i],(unsigned long long)completions[i]);
 if(port_handle)CloseHandle(port_handle);if(complete_event)CloseHandle(complete_event);if(ready_event)CloseHandle(ready_event);if(bye_event)CloseHandle(bye_event);
 WSACleanup();DeleteCriticalSection(&print_lock);
}
static int server(void) {
 SOCKET listener,s;struct sockaddr_in a;fd_set f;struct timeval tv;int i,j,yes=1;uint32_t off,count;unsigned char *body;
 LARGE_INTEGER start;double aggregate_elapsed=0;uint64_t total=0;Peer *p;
 listener=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
 if(listener==INVALID_SOCKET){fail("listen socket",WSAGetLastError());return 0;}
 ZeroMemory(&a,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)cfg.port);InetPtonA(AF_INET,cfg.host,&a.sin_addr);
 setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&yes,sizeof(yes));
 if(bind(listener,(struct sockaddr*)&a,sizeof(a))==SOCKET_ERROR || listen(listener,cfg.workers)==SOCKET_ERROR){fail("bind/listen",WSAGetLastError());closesocket(listener);return 0;}
 logline("LISTEN host=%s port=%d workers=%d server_pid=%lu",cfg.host,cfg.port,cfg.workers,GetCurrentProcessId());
 QueryPerformanceCounter(&start);
 for(i=0;i<cfg.workers;++i){
  FD_ZERO(&f);FD_SET(listener,&f);tv.tv_sec=cfg.timeout;tv.tv_usec=0;
  if(select(0,&f,NULL,NULL,&tv)<=0){fail("accept timeout",WAIT_TIMEOUT);closesocket(listener);return 0;}
  s=accept(listener,NULL,NULL);if(s==INVALID_SOCKET){fail("accept",WSAGetLastError());closesocket(listener);return 0;}
  setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(char*)&yes,sizeof(yes));
  if(!attach(&peers[i],s,i)){closesocket(s);closesocket(listener);return 0;}++peer_count;
 }
 closesocket(listener);
 if(!wait_event(ready_event,"HELLO timeout"))return 0;
 for(i=0;i<cfg.workers;++i)for(j=i+1;j<cfg.workers;++j)if(peers[i].id==peers[j].id){fail("duplicate worker id",peers[i].id);return 0;}
 if(cfg.bench)logline("BENCH_CONFIG direction=%s active=%d payload_per_link=%llu chunk=%u",cfg.up?"up":"down",cfg.active,(unsigned long long)cfg.bytes,cfg.chunk);
 else logline("TASK_SPLIT elements=%u workers=%d deterministic_value=(i*17+23)%%1009",cfg.elements,cfg.workers);
 QueryPerformanceCounter(&benchmark_start);
 for(i=0;i<cfg.workers;++i){
  p=&peers[i];EnterCriticalSection(&p->lock);
  if(cfg.bench && selected(p)) {
   unsigned char command[20];p->seed=0x12340000u+(uint32_t)p->id;
   put32(command,(uint32_t)cfg.up);put64(command+4,cfg.bytes);put32(command+12,cfg.chunk);put32(command+16,p->seed);
   if(cfg.up)p->receiving_bulk=1;else p->bulk_sender=1;
   queue_frame(p,START,p->id,0,p->id,command,20);
  } else if(!cfg.bench) {
   off=(uint32_t)(((uint64_t)cfg.elements*(uint32_t)(p->id-1))/(uint32_t)cfg.workers);
   count=(uint32_t)(((uint64_t)cfg.elements*(uint32_t)p->id)/(uint32_t)cfg.workers)-off;
   p->task_offset=off;p->task_count=count;p->expected=0;
   body=(unsigned char*)malloc(16u+count*4u);if(!body){LeaveCriticalSection(&p->lock);fail("task buffer",ERROR_OUTOFMEMORY);return 0;}
   put64(body,off);put32(body+8,count);put32(body+12,(uint32_t)p->id);
   for(j=0;j<(int)count;++j){uint32_t v=value_at(off+(uint32_t)j);put32(body+16u+(uint32_t)j*4u,v);p->expected+=v;}
   queue_frame(p,TASK,p->id,0,p->id,body,16u+count*4u);free(body);
  }
  LeaveCriticalSection(&p->lock);
 }
 if(!wait_event(complete_event,"run timeout"))return 0;
 if(cfg.bench){
  for(i=0;i<cfg.workers;++i){p=&peers[i];if(!selected(p))continue;
   if(p->elapsed>aggregate_elapsed)aggregate_elapsed=p->elapsed;total+=cfg.bytes;
   logline("CSV,link,%d,%llu,%.9f,%.9f,%.9f",p->id,(unsigned long long)cfg.bytes,p->elapsed,(double)cfg.bytes/p->elapsed/1e9,(double)cfg.bytes/p->elapsed*8.0/1e9);
  }
  logline("CSV,aggregate,%d,%llu,%.9f,%.9f,%.9f",active_count(),(unsigned long long)total,aggregate_elapsed,(double)total/aggregate_elapsed/1e9,(double)total/aggregate_elapsed*8.0/1e9);
  logline("BENCH_CHECK=PASS direction=%s active=%d payload_bytes=%llu errors=0",cfg.up?"up":"down",cfg.active,(unsigned long long)total);
 }else{
  merged_sum=0;reference_sum=0;for(i=0;i<cfg.workers;++i)merged_sum+=peers[i].sum;
  for(off=0;off<cfg.elements;++off)reference_sum+=value_at(off);
  if(merged_sum!=reference_sum){fail("merge mismatch",1);return 0;}
  logline("MERGE elements=%u partials=%d sum=%llu reference=%llu elapsed_s=%.9f check=PASS",cfg.elements,cfg.workers,(unsigned long long)merged_sum,(unsigned long long)reference_sum,since_start());
 }
 for(i=0;i<cfg.workers;++i){EnterCriticalSection(&peers[i].lock);queue_frame(&peers[i],QUIT,0,0,0,NULL,0);LeaveCriticalSection(&peers[i].lock);}
 return wait_event(bye_event,"shutdown acknowledgement timeout");
}
static int worker(void) {
 SOCKET s;struct sockaddr_in a;int i,yes=1;
 s=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
 if(s==INVALID_SOCKET){fail("worker socket",WSAGetLastError());return 0;}
 ZeroMemory(&a,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)cfg.port);InetPtonA(AF_INET,cfg.host,&a.sin_addr);
 for(i=0;i<100;++i){if(connect(s,(struct sockaddr*)&a,sizeof(a))==0)break;Sleep(100);}
 if(i==100){fail("connect",WSAGetLastError());closesocket(s);return 0;}
 setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(char*)&yes,sizeof(yes));
 if(!attach(&peers[0],s,0)){closesocket(s);return 0;}peer_count=1;peers[0].id=cfg.id;
 EnterCriticalSection(&peers[0].lock);queue_frame(&peers[0],HELLO,0,0,cfg.id,NULL,0);LeaveCriticalSection(&peers[0].lock);
 logline("WORKER_CONNECTED id=%d pid=%lu host=%s port=%d",cfg.id,GetCurrentProcessId(),cfg.host,cfg.port);
 return wait_event(complete_event,"worker run timeout");
}
static int options(int argc,char **argv) {
 int i;uint64_t v;char *end;const char *key,*arg;
 ZeroMemory(&cfg,sizeof(cfg));cfg.server=1;cfg.workers=3;cfg.id=1;cfg.threads=2;cfg.port=32103;cfg.timeout=60;cfg.elements=900003;cfg.bytes=256ull*1024ull*1024ull;cfg.chunk=65536;strcpy_s(cfg.host,sizeof(cfg.host),"127.0.0.1");
 for(i=1;i<argc;++i){key=argv[i];if(i+1>=argc){fprintf(stderr,"Missing value: %s\n",key);return 0;}arg=argv[++i];
  if(!strcmp(key,"--role")){if(!strcmp(arg,"server"))cfg.server=1;else if(!strcmp(arg,"worker"))cfg.server=0;else return 0;continue;}
  if(!strcmp(key,"--mode")){if(!strcmp(arg,"task"))cfg.bench=0;else if(!strcmp(arg,"bench"))cfg.bench=1;else return 0;continue;}
  if(!strcmp(key,"--direction")){if(!strcmp(arg,"down"))cfg.up=0;else if(!strcmp(arg,"up"))cfg.up=1;else return 0;continue;}
  if(!strcmp(key,"--host")){if(strcmp(arg,"127.0.0.1")){fprintf(stderr,"This validated lab binds/connects to 127.0.0.1 only.\n");return 0;}continue;}
  if(arg[0]=='-')return 0;errno=0;v=_strtoui64(arg,&end,10);if(errno==ERANGE || !arg[0] || *end)return 0;
  if(strcmp(key,"--bytes") && strcmp(key,"--mib") && v>INT_MAX)return 0;
  if(!strcmp(key,"--workers"))cfg.workers=(int)v;
  else if(!strcmp(key,"--active"))cfg.active=(int)v;
  else if(!strcmp(key,"--id"))cfg.id=(int)v;
  else if(!strcmp(key,"--threads"))cfg.threads=(int)v;
  else if(!strcmp(key,"--port"))cfg.port=(int)v;
  else if(!strcmp(key,"--timeout"))cfg.timeout=(int)v;
  else if(!strcmp(key,"--elements"))cfg.elements=(uint32_t)v;
  else if(!strcmp(key,"--mib")){if(v>4096)return 0;cfg.bytes=v*1024ull*1024ull;}
  else if(!strcmp(key,"--bytes"))cfg.bytes=v;
  else if(!strcmp(key,"--chunk"))cfg.chunk=(uint32_t)v;
  else if(!strcmp(key,"--delay-ms"))cfg.delay_ms=(int)v;
  else if(!strcmp(key,"--io-slice"))cfg.io_slice=(int)v;
  else{fprintf(stderr,"Unknown option: %s\n",key);return 0;}
 }
 if(cfg.workers<1||cfg.workers>MAX_PEERS||cfg.id<1||cfg.id>MAX_PEERS||cfg.threads<2||cfg.threads>MAX_POOL||cfg.port<1024||cfg.port>65535||cfg.active<0||cfg.active>cfg.workers||cfg.timeout<1||cfg.timeout>600||cfg.delay_ms<0||cfg.delay_ms>10000||cfg.io_slice<0||cfg.io_slice>262144||cfg.elements<1||cfg.elements>1000003||cfg.chunk<4||cfg.chunk>262144||cfg.chunk%4||!cfg.bytes||cfg.bytes%4||cfg.bytes>4ull*1024ull*1024ull*1024ull)return 0;
 return 1;
}
int main(int argc,char **argv) {
 WSADATA ws;int i,ok;
 if(!options(argc,argv)){fprintf(stderr,"Usage: socket_lab --role server|worker --mode task|bench --workers 3 --id 1 --port 32103 --threads 2 --elements 1000003 --direction down|up --active 0 --mib 256 --chunk 65536 --io-slice 0\n");return 2;}
 InitializeCriticalSection(&print_lock);QueryPerformanceFrequency(&frequency);
 complete_event=CreateEventA(NULL,TRUE,FALSE,NULL);ready_event=CreateEventA(NULL,TRUE,FALSE,NULL);bye_event=CreateEventA(NULL,TRUE,FALSE,NULL);
 if(WSAStartup(MAKEWORD(2,2),&ws)){logline("WSAStartup failed");return 2;}
 port_handle=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,(DWORD)cfg.threads);
 if(!port_handle || !complete_event||!ready_event||!bye_event){logline("Initial allocation failed");return 2;}
 logline("SOCKET LAB role=%s pid=%lu iocp_threads=%d io_slice=%d",cfg.server?"server":"worker",GetCurrentProcessId(),cfg.threads,cfg.io_slice);
 for(i=0;i<cfg.threads;++i){threads[i]=CreateThread(NULL,0,pool_thread,(LPVOID)(INT_PTR)i,0,NULL);if(!threads[i]){fail("CreateThread",GetLastError());return 2;}}
 ok=cfg.server?server():worker();if(!ok)InterlockedExchange(&failed,1);
 logline("RESULT=%s role=%s pid=%lu",failed?"FAIL":"PASS",cfg.server?"server":"worker",GetCurrentProcessId());
 clean();return failed?1:0;
}
