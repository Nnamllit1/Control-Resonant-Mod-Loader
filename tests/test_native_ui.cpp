// Include the implementation to test private response ownership and the actual
// assembly forwarding paths without exporting a production testing interface.
#include "../runtime/diagnostics/native_ui.cpp"
#include <iostream>
#include <stdexcept>

namespace crml::native_ui {
namespace {
#define CHECK(condition) do { if(!(condition)) throw std::runtime_error("native UI check failed at line " + std::to_string(__LINE__) + ": " #condition); } while(false)
struct Response {
    void** table{};
    std::vector<char> bytes;
    unsigned buffers{}, finishes{}, destructors{}, unknown_calls{};
    size_t buffer_size_at_unknown{};
    uint32_t status{}, result{}, flags{};
    bool fail_buffer{}, invalidate_on_finish{};
};
void* fake_buffer(Response* response,size_t count) {
    ++response->buffers;
    if(response->fail_buffer) return nullptr;
    response->bytes.resize(count);
    return response->bytes.data();
}
void fake_status(Response* response,uint32_t value) { response->status=value; }
void fake_finish(Response* response,uint32_t value) {
    ++response->finishes;response->result=value;
    if(response->invalidate_on_finish) response->table=nullptr;
}
void* fake_destructor(Response* response,uint32_t flags) {
    ++response->destructors;response->flags=flags;
    return response;
}
uint64_t fake_integer(Response* response,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    ++response->unknown_calls;
    response->buffer_size_at_unknown=response->bytes.size();
    return a+3*b+5*c+7*d+11*e;
}
double fake_float(Response* response,double a,double b,double c,double d,uint64_t e) {
    ++response->unknown_calls;
    response->buffer_size_at_unknown=response->bytes.size();
    return a+3*b+5*c+7*d+11*static_cast<double>(e);
}
void* response_table[]{reinterpret_cast<void*>(&fake_destructor),reinterpret_cast<void*>(&fake_buffer),
    reinterpret_cast<void*>(&fake_integer),reinterpret_cast<void*>(&fake_float),reinterpret_cast<void*>(&fake_integer),
    reinterpret_cast<void*>(&fake_status),reinterpret_cast<void*>(&fake_float),reinterpret_cast<void*>(&fake_integer),
    reinterpret_cast<void*>(&fake_finish)};
template<class F> F slot(void* response,size_t index) {return reinterpret_cast<F>((*static_cast<void***>(response))[index]);}
using Integer=uint64_t(*)(void*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
using Floating=double(*)(void*,double,double,double,double,uint64_t);
using Destructor=void*(*)(void*,uint32_t);
enum class Mode {normal,deferred,unknown_before,unknown_after,float_before,destruct,repeated};
struct Request {
    std::string_view document;
    Mode mode{Mode::normal};
    void* received{};
    void* expected_handler{};
    uint32_t status{200},result{};
    bool duplicate_finish{};
    uint64_t integer_result{};
    double float_result{};
};
void fake_handler(void* handler,void* address,void* response) {
    auto& request=*static_cast<Request*>(address);
    CHECK(handler==request.expected_handler);
    request.received=response;
    if(request.mode==Mode::deferred) return;
    if(request.mode==Mode::destruct) {
        CHECK(slot<Destructor>(response,0)(response,3)!=nullptr);
        return;
    }
    if(request.mode==Mode::unknown_before)
        request.integer_result=slot<Integer>(response,2)(response,1,2,3,4,5);
    if(request.mode==Mode::float_before)
        request.float_result=slot<Floating>(response,3)(response,1.25,2.5,3.75,4.5,5);
    slot<Status>(response,5)(response,request.status);
    auto* target=slot<GetBuffer>(response,1)(response,request.document.size());
    if(target) std::memcpy(target,request.document.data(),request.document.size());
    if(request.mode==Mode::unknown_after)
        request.integer_result=slot<Integer>(response,7)(response,1,2,3,4,5);
    if(request.mode==Mode::repeated) {
        auto* second=slot<GetBuffer>(response,1)(response,request.document.size());
        if(second) std::memcpy(second,request.document.data(),request.document.size());
    }
    slot<Finish>(response,8)(response,request.result);
    if(request.duplicate_finish) slot<Finish>(response,8)(response,request.result);
}
struct Fixture {
    const std::string document="<html><body>Self-authored test document</body></html>";
    const std::string payload="<div id=\"crml-native-ui-v1\">Example</div>";
    std::string digest;
    State owner;
    Response response;
    Request request;
    Fixture() {
        Sha256 hash; CHECK(hash.add(document.data(),document.size()));digest=hash.finish();CHECK(digest.size()==64);
        owner.expected_html_sha=digest;owner.payload=payload;owner.original=&fake_handler;
        response.table=response_table;request.document=document;request.expected_handler=this;
    }
    void run(bool selected=true) {intercept(owner,this,&request,&response,selected);}
    std::string bytes() const {return {response.bytes.begin(),response.bytes.end()};}
};
void test_documents() {
    CHECK(target_url(route));CHECK(target_url(std::string(route)+"?source=options#page"));
    CHECK(!target_url("https://base/uiresources/game/ui/ui.html"));
    CHECK(!target_url(std::string(route)+"/extra"));
    CHECK(!target_url(std::string(route)+std::string("\0extra",6)));
    std::string result="old";
    CHECK(insert_panel("<body>Before</body>","<div>Panel</div>",result));
    CHECK(result=="<body>Before<div>Panel</div></body>");
    CHECK(!insert_panel("<body>Before", "payload",result));CHECK(result.empty());
    CHECK(!insert_panel("</body></body>","payload",result));
    CHECK(!insert_panel("crml-native-ui-v1</body>","payload",result));
    CHECK(!insert_panel(std::string("</body>\0",8),"payload",result));
    CHECK(!insert_panel("</body>",std::string("x\0",2),result));
    CHECK(!insert_panel("</body>",std::string(max_payload+1,'x'),result));
    CHECK(!insert_panel(std::string(max_document+1,'x'),"payload",result));
}
void test_response() {
    { Fixture f;f.response.invalidate_on_finish=true;f.run();
      CHECK(f.response.finishes==1 && f.response.result==0 && f.response.buffers==1);
      CHECK(f.owner.transformed==1 && f.owner.pending==0 && f.owner.completed==1);
      std::string expected;CHECK(insert_panel(f.document,f.payload,expected));CHECK(f.bytes()==expected); }
    { Fixture f;f.request.duplicate_finish=true;f.run();CHECK(f.response.finishes==1 && f.owner.pending==0); }
    { Fixture f;f.run(false);CHECK(f.request.received==&f.response);CHECK(f.owner.requests==0);CHECK(f.bytes()==f.document); }
    { Fixture f;f.owner.expected_html_sha="different";f.run();CHECK(f.bytes()==f.document && f.owner.transformed==0); }
    { Fixture f;f.request.status=404;f.run();CHECK(f.bytes()==f.document && f.response.status==404); }
    { Fixture f;f.request.result=1;f.run();CHECK(f.bytes()==f.document && f.response.result==1); }
    { Fixture f;f.response.fail_buffer=true;f.run();CHECK(f.response.finishes==1 && f.response.result==1 && f.owner.failures==1 && f.owner.transformed==0); }
    for(const auto mode:{Mode::unknown_before,Mode::unknown_after,Mode::float_before,Mode::repeated}) {
        Fixture f;f.request.mode=mode;f.run();CHECK(f.bytes()==f.document && f.owner.transformed==0 && f.owner.pending==0);
        if(mode==Mode::unknown_before || mode==Mode::unknown_after) CHECK(f.request.integer_result==105);
        if(mode==Mode::unknown_after) CHECK(f.response.buffer_size_at_unknown==f.document.size());
        if(mode==Mode::float_before) CHECK(f.request.float_result==114.0);
    }
    { Fixture f;f.response.fail_buffer=true;f.request.mode=Mode::unknown_after;f.run();
      CHECK(f.response.unknown_calls==1 && f.request.integer_result==105);
      CHECK(f.response.result==1 && f.response.finishes==1 && f.owner.pending==0); }
    { Fixture f;f.request.mode=Mode::destruct;f.run();
      CHECK(f.response.destructors==1 && f.response.flags==3 && f.owner.pending==0 && f.response.finishes==0); }
    { Fixture f;f.request.mode=Mode::deferred;f.run();CHECK(f.owner.pending==1 && f.response.finishes==0);
      slot<Finish>(f.request.received,8)(f.request.received,1);CHECK(f.owner.pending==0 && f.response.finishes==1); }
    { Fixture f;f.request.mode=Mode::deferred;f.run();
      CHECK(slot<Integer>(f.request.received,4)(f.request.received,1,2,3,4,5)==105);
      CHECK(slot<Floating>(f.request.received,6)(f.request.received,1.25,2.5,3.75,4.5,5)==114.0);
      slot<Finish>(f.request.received,8)(f.request.received,1);CHECK(f.owner.pending==0); }
    { Fixture f;f.request.mode=Mode::deferred;f.run();CHECK(f.owner.pending==1);
      CHECK(slot<Destructor>(f.request.received,0)(f.request.received,7)==&f.response);
      CHECK(f.owner.pending==0 && f.response.flags==7); }
    { Fixture f;std::array<Response,5> responses;std::array<Request,5> requests;
      for(size_t i=0;i<5;++i) {responses[i].table=response_table;requests[i].mode=Mode::deferred;requests[i].expected_handler=&f;
        intercept(f.owner,&f,&requests[i],&responses[i],true);}
      CHECK(f.owner.pending==4 && f.owner.exhausted==1 && requests[4].received==&responses[4]);
      for(size_t i=0;i<5;++i) slot<Finish>(requests[i].received,8)(requests[i].received,1);
      CHECK(f.owner.pending==0 && f.owner.completed==4);
      for(const auto& response:responses) CHECK(response.finishes==1);
    }
}
void test_ui_exchange_response() {
    ui::Service bridge;bridge.enable(true);const auto page=bridge.open_page();CHECK(page!=0);
    const auto prefix=std::string(ui::poll_prefix)+std::to_string(page)+"/";
    {Response response;response.table=response_table;
     CHECK(!serve_ui(bridge,"coui://base/unrelated",&response,100));
     CHECK(response.finishes==0 && response.buffers==0);}
    {Response response;response.table=response_table;response.invalidate_on_finish=true;
     CHECK(serve_ui(bridge,prefix+"1/1/1/0",&response,100));
     CHECK(response.status==200 && response.result==0 && response.finishes==1 && response.buffers==1);
     CHECK(std::string(response.bytes.begin(),response.bytes.end()).find("\"id\":0")!=std::string::npos);}
    crml_ui_state snapshot{};CHECK(bridge.read_at(snapshot,100)==1);
    CHECK(bridge.activate_at(7,snapshot.generation,1,100)==0);
    {Response response;response.table=response_table;response.fail_buffer=true;
     CHECK(serve_ui(bridge,prefix+"2/1/1/0",&response,100));
     CHECK(response.status==200 && response.finishes==1 && response.result==1 && response.buffers==1);}
    {Response response;response.table=response_table;
     CHECK(serve_ui(bridge,prefix+"3/1/1/0",&response,100));
     CHECK(std::string(response.bytes.begin(),response.bytes.end()).find("\"id\":0")!=std::string::npos);
     CHECK(bridge.acknowledgements()==0);}
    for(const auto& [suffix,code]:std::array<std::pair<const char*,uint32_t>,3>{{
        {"bad",400},{"3/1/1/0",409},{"4/1/1/99",409}}}) {
        Response response;response.table=response_table;response.invalidate_on_finish=true;
        CHECK(serve_ui(bridge,prefix+suffix,&response,100));
        CHECK(response.status==code && response.finishes==1 && response.result==0 && response.buffers==0);
    }
    bridge.enable(false);
    {Response response;response.table=response_table;
     CHECK(serve_ui(bridge,prefix+"4/1/1/0",&response,100));
     CHECK(response.status==503 && response.finishes==1 && response.buffers==0);}
}
void test_ui_page_nonce() {
    ui::Service bridge;bridge.enable(true);
    {Fixture f;f.owner.bridge=&bridge;f.owner.payload="<script>var page='__CRML_UI_NONCE__';</script>";f.run();
     CHECK(f.owner.transformed==1 && f.bytes().find("var page='1'")!=std::string::npos);
     CHECK(f.bytes().find("__CRML_UI_NONCE__")==std::string::npos);}
    std::string body;CHECK(bridge.exchange(std::string(ui::poll_prefix)+"1/1/1/1/0",100,body)==200);
    crml_ui_state snapshot{};CHECK(bridge.read_at(snapshot,100)==1);
    CHECK(bridge.activate_at(7,snapshot.generation,1,100)==0);
    {Fixture f;f.owner.bridge=&bridge;f.owner.payload="<script>var page='__CRML_UI_NONCE__';</script>";f.run();
     CHECK(f.owner.transformed==1 && f.bytes().find("var page='2'")!=std::string::npos);}
    CHECK(bridge.read_at(snapshot,100)<0);
    CHECK(bridge.exchange(std::string(ui::poll_prefix)+"1/2/1/1/0",100,body)==409);
    CHECK(bridge.exchange(std::string(ui::poll_prefix)+"2/1/1/1/0",100,body)==200);
    CHECK(body.find("\"id\":0")!=std::string::npos);
    {Fixture f;f.owner.bridge=&bridge;f.run();CHECK(f.owner.transformed==0 && f.bytes()==f.document);}
    bridge.enable(false);
    {Fixture f;f.owner.bridge=&bridge;f.owner.payload="<script>var page='__CRML_UI_NONCE__';</script>";f.run();
     CHECK(f.owner.transformed==0 && f.bytes()==f.document);}
}
}
int run_tests() {test_documents();test_response();test_ui_exchange_response();test_ui_page_nonce();return 0;}
}
int main() {
    try {const auto result=crml::native_ui::run_tests();std::cout<<"Native UI route, response ABI, transformation and lifetime checks passed\n";return result;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
