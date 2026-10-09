#include "session_file.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs=std::filesystem;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string read(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),{}};
}
void put(const fs::path& path,const std::string& bytes) {std::ofstream(path,std::ios::binary)<<bytes;}
struct Temporary {
    fs::path root=fs::current_path()/(L"session-file-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Temporary() {require(fs::create_directory(root),"create private fixture directory");}
    ~Temporary() {std::error_code error;fs::remove_all(root,error);}
    fs::path directory(const char* name) {auto path=root/name;fs::create_directory(path);return path;}
};
int main() {
    try {
        Temporary fixture;
        const auto root=fixture.directory("history");
        for(int session=1;session<=6;++session) {
            crml::SessionFile file(root);
            if(!file.available() || !file.rotated())std::cerr<<"session="<<session<<" available="<<file.available()<<" win32="<<GetLastError()<<'\n';
            require(file.available() && file.rotated(),"session starts and rotates");
            file.stream()<<"session "<<session<<'\n';file.stream().flush();
            require(read(root/"crml.log")=="session "+std::to_string(session)+"\n","current log readable during session");
            crml::SessionFile conflicting(root);
            require(!conflicting.available(),"second writer refused");
            require(read(root/"crml.log")=="session "+std::to_string(session)+"\n","second writer preserves current file");
        }
        for(int history=0;history<=3;++history) {
            const auto name=history?"crml."+std::to_string(history)+".log":"crml.log";
            require(read(root/name)=="session "+std::to_string(6-history)+"\n","bounded chronological history");
        }
        require(!fs::exists(root/"crml.4.log"),"no unbounded history");
        const auto reader=CreateFileW((root/"crml.log").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(reader!=INVALID_HANDLE_VALUE,"reader allows writes but denies renaming");
        for(int attempt=0;attempt<2;++attempt) {
            crml::SessionFile file(root);
            require(file.available() && !file.rotated(),"held current reader uses append fallback");
            file.stream()<<"another session\n";
            for(int history=1;history<=3;++history)
                require(read(root/("crml."+std::to_string(history)+".log"))=="session "+std::to_string(6-history)+"\n",
                    "failed rotation preserves all previous sessions");
        }
        CloseHandle(reader);
        const auto blocked=fixture.directory("blocked");
        put(blocked/"crml.log","previous evidence\n");fs::create_directory(blocked/"crml.1.log");
        {
            crml::SessionFile file(blocked);
            require(file.available() && !file.rotated(),"history obstruction uses append fallback");
            file.stream()<<"new evidence\n";
        }
        const auto combined=read(blocked/"crml.log");
        require(combined.starts_with("previous evidence\n") && combined.ends_with("new evidence\n"),"append preserves both sessions");
        require(fs::is_directory(blocked/"crml.1.log"),"obstructing directory retained");
        const auto links=fixture.directory("links");
        put(links/"outside.txt","untouched");
        require(CreateHardLinkW((links/"crml.log").c_str(),(links/"outside.txt").c_str(),nullptr)!=0,"create hardlink fixture");
        {crml::SessionFile file(links);require(!file.available(),"linked current log refused");}
        require(read(links/"outside.txt")=="untouched" && !fs::exists(links/"crml.1.log"),"linked file not rotated or truncated");
        const auto history_links=fixture.directory("history-links");
        put(history_links/"outside.txt","untouched");put(history_links/"crml.log","old\n");
        require(CreateHardLinkW((history_links/"crml.2.log").c_str(),(history_links/"outside.txt").c_str(),nullptr)!=0,"create history hardlink");
        {crml::SessionFile file(history_links);require(file.available() && !file.rotated(),"linked history falls back");file.stream()<<"new\n";}
        require(read(history_links/"outside.txt")=="untouched","history target untouched");
        const auto bounded=fixture.directory("bounded");
        {crml::SessionFile file(bounded);require(file.available(),"bounded session starts");file.stream()<<std::string(crml::SessionFile::limit+100,'x');}
        require(fs::file_size(bounded/"crml.log")==crml::SessionFile::limit,"whole file cap");
        require(read(bounded/"crml.log").ends_with("later output is suppressed.\n"),"cap explains suppression");
        std::cout<<"Session history, readers, writer exclusion, fallback, linked files and size limits passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
