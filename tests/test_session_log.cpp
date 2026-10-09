#include "session_log.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
int main() {
    try {
        std::ostringstream stream;uint64_t now=100;
        crml::SessionLog log(stream,[&]{return now;});
        log.guest("spam",0,"debug\nmessage");
        now=123;log.guest("spam",3,"error message");
        require(stream.str().find("[+23ms] [spam] [error] error message")!=std::string::npos,"severity and elapsed time");
        require(stream.str().find("debug message")!=std::string::npos,"sanitize guest newline");
        for(int i=0;i<2000;++i) log.guest("spam",1,std::string(4096,'x'));
        const auto capped=stream.str();
        log.guest("spam",1,"short line after exhaustion");
        require(stream.str()==capped,"smaller messages must not resume a suppressed session quota");
        log.guest("healthy",2,"still visible");
        log.host("Disabled failed-mod: trap");
        require(stream.str().find("[healthy] [warning] still visible")!=std::string::npos,"independent mod quota");
        require(stream.str().find("[host] Disabled failed-mod: trap")!=std::string::npos,"reserved host quota");
        require(stream.str().size()<crml::SessionLog::guest_limit+1024,"flood stays bounded");
        std::cout<<"Independent diagnostic quotas, severity and timestamp checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
