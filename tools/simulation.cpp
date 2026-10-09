#include "simulation.h"
#include "runtime.h"
#include "action_bindings.h"
#include "session_log.h"
#include "mod_feedback.h"
#include "mod_settings.h"
#include "mod_lists.h"
#include "noclip.h"
#include "visibility.h"
#include "ui_service.h"
#include "media_service.h"
#include <cmath>
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
constexpr uint32_t supported = CRML_CAP_INPUT_BUTTONS | CRML_CAP_PLAYER_VISIBILITY |
    CRML_CAP_PHYSICS_DAMPING | CRML_CAP_INPUT_MOTION | CRML_CAP_PLAYER_MOTION |
    CRML_CAP_INPUT_ACTIONS | CRML_CAP_PLAYER_READ | CRML_CAP_CAMERA_READ | CRML_CAP_FEEDBACK |
    CRML_CAP_UI_READ | CRML_CAP_UI_ACTIVATE | CRML_CAP_UI_PRESENTATION |
    CRML_CAP_MEDIA_READ | CRML_CAP_MEDIA_SKIP | CRML_CAP_NAVIGATION_READ | CRML_CAP_LISTS;
struct Provider final : crml::Gameplay, crml::Input {
    uint64_t now{}, frame{}, physics_owner{}, token_counter{};
    crml::probe::Flight flight;
    crml::probe::visibility::Lease visibility;
    uint64_t visibility_expired_epoch{};
    uint32_t visibility_observation{};
    uint32_t caps{supported};
    bool focused{true}, fresh{true}, emergency{}, overflow{};
    size_t events{};
    std::set<uint16_t> keys;
    std::map<std::string,int> outcomes;
    int player_result{}, navigation_result{}, camera_result{}, physics_result{}, heading_result{}, status{};
    crml_player_state player{};
    crml_navigation_state navigation{};
    crml_camera_state camera{};
    crml_physics_state physics{};
    float heading[2]{};
    crml::ui::Service ui;
    crml::media::Service media;
    uintptr_t media_identity{};
    uint32_t media_elapsed{},media_source{CRML_MEDIA_ENGINE_NAME};
    bool media_active{},media_ready{},media_consume{true},media_observer{true};
    std::string media_name;
    uint64_t ui_page{};
    uint32_t ui_sequence{},ui_ack{},ui_ack_outcome{},ui_screen{},ui_actions{},ui_outcome{1};
    bool ui_configured{},ui_renderer{true};

    void observe_visibility() noexcept {
        if((caps&CRML_CAP_PLAYER_VISIBILITY) && visibility_observation)
            visibility.observe(visibility.observation_token(now),visibility_observation==2);
    }

    void observe_media() noexcept {
        if(media_observer && media.observe_at(media_identity,media_active,media_ready,media_elapsed,
                                               media_name,media_source,now,media_consume))
            event("media_consumed",0,0,media_identity);
    }

    void new_ui_page() noexcept {
        ui_page=ui.open_page();ui_sequence=ui_ack=ui_ack_outcome=0;
    }
    void render_ui() {
        if(!ui_page || !ui_configured || !ui_renderer) return;
        std::string response;
        const auto url=std::string(crml::ui::result_poll_prefix)+std::to_string(ui_page)+"/"+
            std::to_string(++ui_sequence)+"/"+std::to_string(ui_screen)+"/"+std::to_string(ui_actions)+"/"+
            std::to_string(ui_ack)+"/"+std::to_string(ui_ack_outcome);
        if(ui.exchange(url,now,response)!=200) throw std::runtime_error("Simulation UI renderer rejected a poll");
        const auto pos=response.find("\"id\":");
        if(pos==response.npos) throw std::runtime_error("Simulation UI response missing command ID");
        const auto id=static_cast<uint32_t>(std::stoul(response.substr(pos+5)));
        if(id && ui_outcome) {ui_ack=id;ui_ack_outcome=ui_outcome;}
    }

    bool ready(uint32_t cap) const noexcept { return (caps & cap) && focused && !emergency; }
    uint32_t capabilities() const noexcept override { return caps; }
    bool available() noexcept override { return (caps & CRML_CAP_INPUT_ACTIONS) != 0; }
    uint32_t state_flags() noexcept override {
        return (available()?CRML_INPUT_AVAILABLE:0u)|CRML_INPUT_CONTEXT_KNOWN|
            (focused?CRML_INPUT_FOCUSED:0u)|(fresh?CRML_INPUT_FRESH:0u)|(emergency?CRML_INPUT_EMERGENCY:0u);
    }
    bool down(uint16_t key) const noexcept { return focused && fresh && !emergency && keys.count(key) != 0; }
    uint32_t sample(const std::array<uint16_t,CRML_ACTION_COUNT>& bindings) noexcept override {
        uint32_t bits{};
        if (available()) for(size_t i=0;i<bindings.size();++i) if(down(bindings[i])) bits |= 1u<<i;
        return bits;
    }
    uint32_t input_buttons() noexcept override {
        return ready(CRML_CAP_INPUT_BUTTONS) ? uint32_t(down(0x76)) | (uint32_t(down(0x77))<<1) : 0;
    }
    uint32_t input_motion() noexcept override {
        if (!ready(CRML_CAP_INPUT_MOTION)) return 0;
        constexpr uint16_t controls[]{0x75,'W','S','A','D',0x20,0x11,0x10};
        uint32_t bits{};
        for(size_t i=0;i<std::size(controls);++i) if(down(controls[i])) bits |= 1u<<i;
        return bits;
    }
    int outcome(const char* operation, int normal) const noexcept {
        const auto found=outcomes.find(operation);return found==outcomes.end()?normal:found->second;
    }
    template<class... Args> void event(const char* operation,uint64_t owner,int64_t result,Args... args) noexcept {
        if(++events>50000) {overflow=true;return;}
        std::cout<<"@event "<<frame<<' '<<now<<' '<<operation<<' '<<owner<<' '<<result;
        ((std::cout<<' '<<args),...);std::cout<<'\n';
    }
    void expire() noexcept {
        if(flight.owner && (!focused || !fresh || emergency || now-flight.lease>500)) {
            event("motion_cancel",flight.owner,0);
            flight.reset(!focused?crml::probe::StopReason::focus:!fresh?crml::probe::StopReason::stale_sample:
                emergency?crml::probe::StopReason::escape:crml::probe::StopReason::lease,now);
        }
        if(visibility.owner && visibility.deadline.load() && (emergency || !visibility.active(now))) {
            const auto epoch=visibility.epoch.load();
            if(emergency || visibility_expired_epoch!=epoch) event("visibility_cancel",visibility.owner,0);
            visibility_expired_epoch=epoch;
            if(emergency) visibility.release(visibility.owner);
        }
        if(physics_owner && (!focused || emergency)) {event("physics_cancel",physics_owner,0);physics_owner=0;}
    }
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    int ui_read(crml_ui_state& out) noexcept override {
        if(!(caps&CRML_CAP_UI_READ)) {out={};return -1;}
        return ui.read_at(out,now);
    }
    int ui_activate(uint64_t owner,uint64_t generation,uint32_t action) noexcept override {
        const auto result=(caps&CRML_CAP_UI_ACTIVATE)?ui.activate_at(owner,generation,action,now):-1;
        event("ui_activate",owner,result,generation,action);return result;
    }
    int64_t ui_action_submit(uint64_t owner,uint64_t generation,uint32_t action) noexcept override {
        const auto result=(caps&CRML_CAP_UI_ACTIVATE)?ui.submit_at(owner,generation,action,now):-1;
        event("ui_action_submit",owner,result,generation,action);return result;
    }
    int ui_action_status(uint64_t owner,uint64_t ticket) noexcept override {
        return (caps&CRML_CAP_UI_ACTIVATE)?ui.status_at(owner,ticket,now):-1;
    }
    int ui_present(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration) noexcept override {
        const auto result=(caps&CRML_CAP_UI_PRESENTATION)?ui.present_at(owner,generation,kind,name,hidden,duration,now):-1;
        // Encode the copied target, including rejected names, without allowing
        // guest bytes to inject native trace records.
        std::array<char,129> target{};
        constexpr char hex[]="0123456789abcdef";
        const auto count=std::min(name.size(),size_t(64));
        for(size_t i=0;i<count;++i) {
            const auto c=static_cast<unsigned char>(name[i]);
            target[i*2]=hex[c>>4];target[i*2+1]=hex[c&15];
        }
        if(!count) target[0]='-';
        event("ui_present",owner,result,generation,kind,int(hidden),duration,target.data());return result;
    }
    int media_read(crml_media_state& out) noexcept override {
        if(!(caps&CRML_CAP_MEDIA_READ)) {out={};return -1;}
        return media.read_at(out,now);
    }
    int media_skip(uint64_t owner,uint64_t generation) noexcept override {
        const auto result=(caps&CRML_CAP_MEDIA_SKIP)?media.request_at(owner,generation,now):-1;
        event("media_skip",owner,result,generation);return result;
    }
    int motion_camera(float (&out)[2]) noexcept override {
        out[0]=out[1]=0;
        if(!ready(CRML_CAP_PLAYER_MOTION)) return -1;
        if(heading_result==1) {out[0]=heading[0];out[1]=heading[1];}
        return heading_result;
    }
    int motion_read(uint64_t owner,crml_motion_state& out) noexcept override {
        out={};if(!(caps&CRML_CAP_PLAYER_MOTION)) return -1;
        return flight.read_motion(owner,out);
    }
    int motion_set(uint64_t owner,bool enabled,float x,float y,float z) noexcept override {
        expire();
        int result=-1;
        if((caps & CRML_CAP_PLAYER_MOTION) && (!enabled || (focused && !emergency))) {
            // Reuse the real pure state machine for lease expiry, pending
            // cancellation and ownership. No controller callback or game memory.
            auto candidate=flight;
            crml::probe::Sample sample{};sample.entity=1;sample.world=1;
            result=candidate.request_motion(owner,enabled,x,y,z,sample,now,fresh);
            if(result<0) flight=candidate;
            else {
                result=outcome("motion_set",result);
                if(result>=0) flight=candidate;
            }
        }
        event("motion_set",owner,result,int(enabled),x,y,z);return result;
    }
    int visibility_read(uint64_t owner,crml_visibility_state& out) noexcept override {
        out={};if(!(caps&CRML_CAP_PLAYER_VISIBILITY)) return -1;
        return visibility.read_state(owner,now,out);
    }
    int visibility_set(uint64_t owner,bool hidden) noexcept override {
        expire();
        int result=!(caps & CRML_CAP_PLAYER_VISIBILITY)?-1:
            visibility.owner && visibility.owner!=owner && visibility.active(now)?-2:0;
        if(result>=0) result=outcome("visibility_set",result);
        if(result>=0) result=visibility.renew(owner,hidden && focused && !emergency,now);
        event("visibility_set",owner,result,int(hidden));return result;
    }
    int player_read(crml_player_state& out) noexcept override {
        out={};if(!(caps & CRML_CAP_PLAYER_READ)) return -1;
        if(!focused || emergency) return 0;
        if(player_result==1) out=player;return player_result;
    }
    int navigation_read(crml_navigation_state& out) noexcept override {
        out={};if(!(caps & CRML_CAP_NAVIGATION_READ)) return -1;
        if(!focused || emergency || navigation.age_ms>500) return 0;
        if(navigation_result==1) out=navigation;return navigation_result;
    }
    int camera_read(crml_camera_state& out) noexcept override {
        out={};if(!(caps & CRML_CAP_CAMERA_READ)) return -1;
        if(camera_result==1) out=camera;return camera_result;
    }
    int physics_select(uint64_t owner) noexcept override {return physics_select_near(owner,0,0,0,2);}
    int physics_select_near(uint64_t owner,float x,float y,float z,float radius) noexcept override {
        int result=!ready(CRML_CAP_PHYSICS_DAMPING)?-1:physics_owner && physics_owner!=owner?-2:0;
        if(result>=0) result=outcome("physics_select",0);
        if(result==0) {physics_owner=owner;++token_counter;}
        event("physics_select",owner,result,x,y,z,radius);return result;
    }
    int owner_status(uint64_t owner) const noexcept {
        return !(caps & CRML_CAP_PHYSICS_DAMPING)?-1:!physics_owner?0:physics_owner!=owner?-2:status;
    }
    uint64_t physics_target(uint64_t owner) noexcept override {
        return physics_owner==owner && owner_status(owner)==3?token_counter:0;
    }
    int physics_status(uint64_t owner) noexcept override {return owner_status(owner);}
    int physics_apply(uint64_t owner,uint64_t token,float damping,uint32_t duration) noexcept override {
        int result=!focused || emergency?-1:owner_status(owner);
        if(result>=0) result=physics_owner!=owner || token!=token_counter?-3:status!=3?-2:outcome("physics_apply",0);
        event("physics_apply",owner,result,token,damping,duration);return result;
    }
    int physics_restore(uint64_t owner) noexcept override {
        int result=!(caps & CRML_CAP_PHYSICS_DAMPING)?-1:physics_owner && physics_owner!=owner?-2:outcome("physics_restore",0);
        if(result==0 && physics_owner==owner) physics_owner=0;
        event("physics_restore",owner,result);return result;
    }
    int physics_read(uint64_t owner,uint64_t token,crml_physics_state& out) noexcept override {
        out={};if(!focused || emergency) return -1;const auto state=owner_status(owner);if(state<0) return state;
        if(physics_owner!=owner || token!=token_counter) return -3;
        if(status==1 || status==2 || status==5) return -2;
        if(status==7 || status==8 || status==9) return -3;
        if(physics_result==1) out=physics;return physics_result;
    }
    void release(uint64_t owner) noexcept override {
        ui.release(owner);
        media.release(owner);
        // Trace automatic cleanup only when there is something to release.
        // Runtime may call this more than once; ownership cleanup is idempotent.
        if(flight.owner==owner || (visibility.owner==owner && visibility.active(now)) || physics_owner==owner) event("release",owner,0);
        flight.release_owner(owner,now);
        visibility.release(owner);
        if(physics_owner==owner) physics_owner=0;
    }
};

template<class T> void value(std::istringstream& input,T& out) {
    if(!(input>>out)) throw std::runtime_error("Invalid simulation value");
}
template<> void value(std::istringstream& input,float& out) {
    if(!(input>>out) || !std::isfinite(out)) throw std::runtime_error("Expected finite simulation value");
}
void result(std::istringstream& input,int& out,int minimum=-1) {
    value(input,out);if(out<minimum || out>1) throw std::runtime_error("Invalid snapshot result");
}
void boolean(std::istringstream& input,bool& out) {
    int number{};value(input,number);if(number!=0 && number!=1) throw std::runtime_error("Expected 0 or 1");out=number!=0;
}
}

int simulate(const std::filesystem::path& mods,bool profile) {
    Provider provider;
    std::cout<<std::setprecision(std::numeric_limits<float>::max_digits10);
    crml::SessionLog log(std::cout,[&]{return provider.now;});
    crml::ModFeedback feedback([&]{return provider.now;});
    crml::ModSettings settings;
    crml::ModLists lists([&]{return provider.now;});
    bool lists_renderer=true;
    struct ListAction {std::string mod;uint64_t row{},revision{};};
    std::vector<ListAction> list_actions;
    std::map<uint64_t,uint64_t> list_pages;
    const auto activate_lists=[&] {
        for(const auto& action:list_actions) {
            const auto groups=lists.snapshot();
            const auto found=std::find_if(groups.begin(),groups.end(),[&](const auto& group){return group.id==action.mod;});
            if(found==groups.end())throw std::runtime_error("No published simulation list: "+action.mod);
            const auto revision=action.revision?action.revision:found->revision;
            const auto status=lists.activate(found->owner,revision,action.row);
            provider.event("list_activate",found->owner,status,revision,action.row);
        }
        list_actions.clear();
    };
    const auto render_lists=[&] {
        std::map<uint64_t,uint64_t> current;
        for(const auto& group:lists.snapshot()) {
            current.emplace(group.owner,group.revision);
            const auto previous=list_pages.find(group.owner);
            if(previous==list_pages.end() || previous->second!=group.revision) {
                uint64_t selected{};
                for(uint32_t i=0;i<group.page.row_count;++i)if(group.page.rows[i].flags&CRML_LIST_ROW_SELECTED)selected=group.page.rows[i].id;
                provider.event("list_page",group.owner,1,group.revision,group.page.row_count,selected);
            }
        }
        for(const auto& [owner,revision]:list_pages)if(!current.contains(owner))provider.event("list_page",owner,0,revision,0,0);
        list_pages=std::move(current);
    };
    struct SettingEdit {std::string mod,key;double value{};bool is_text{};std::string text;};
    std::vector<SettingEdit> initial_settings;
    const auto edit_setting=[&](const SettingEdit& edit) {
        for(const auto& group:settings.snapshot()) if(group.id==edit.mod) {
            for(const auto& item:group.items) if(edit.key==item.definition.key) {
                const auto result=edit.is_text?settings.set_text(group.owner,item.state.handle,edit.text,item.state.revision):
                    settings.set(group.owner,item.state.handle,edit.value,item.state.revision);
                if(result<0)
                    throw std::runtime_error("Simulation setting value rejected: "+edit.mod+"/"+edit.key);
                return;
            }
        }
        throw std::runtime_error("Unknown simulation setting: "+edit.mod+"/"+edit.key);
    };
    uint64_t feedback_page{},feedback_sequence{};
    bool feedback_renderer=true;
    std::vector<uint64_t> feedback_ack;
    const auto render_feedback=[&] {
        if(!feedback_page || !feedback_renderer)return;
        std::vector<crml::ModFeedback::Message> messages;
        if(feedback.poll(feedback_page,++feedback_sequence,feedback_ack,messages)!=200)
            throw std::runtime_error("Simulation feedback renderer rejected a poll");
        feedback_ack.clear();for(const auto& message:messages)feedback_ack.push_back(message.ticket);
    };
    crml::Runtime runtime([&](const std::string& text){log.host(text);},&provider,&provider,
        [&](std::string_view id,int level,std::string_view text){log.guest(id,level,text);},nullptr,&settings,&feedback,[&]{return provider.now;},nullptr,nullptr,&lists);
    bool started=false,finished=false;size_t lines{};
    for(std::string line;std::getline(std::cin,line);) {
        if(++lines>200000 || line.size()>4096) throw std::runtime_error("Simulation input limit exceeded");
        std::istringstream input(line);std::string command;value(input,command);
        if(finished) throw std::runtime_error("Command after simulation finish");
        if(command=="caps") {
            value(input,provider.caps);
            if(started || (provider.caps & ~supported)) throw std::runtime_error("Unsupported simulation capabilities or late change");
        } else if(command=="list_action") {
            ListAction action;value(input,action.mod);
            const auto identifier=[&](uint64_t& output) {
                std::string token;value(input,token);
                const auto parsed=std::from_chars(token.data(),token.data()+token.size(),output);
                if(token.empty() || token[0]<'0' || token[0]>'9' || parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size())
                    throw std::runtime_error("Invalid simulation list integer");
            };
            identifier(action.row);identifier(action.revision);
            if(action.mod.empty() || action.mod.size()>64 || action.mod.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=action.mod.npos || !action.row || list_actions.size()>=16)
                throw std::runtime_error("Invalid or excessive simulation list action");
            list_actions.push_back(std::move(action));
        } else if(command=="lists_renderer") {
            boolean(input,lists_renderer);lists.enable_renderer(lists_renderer && (provider.caps&CRML_CAP_LISTS));
        } else if(command=="setting" || command=="setting_text") {
            SettingEdit edit;value(input,edit.mod);value(input,edit.key);edit.is_text=command=="setting_text";
            if(edit.is_text) {
                std::string encoded;value(input,encoded);
                if(encoded.empty() || encoded[0]!='t' || encoded.size()%2!=1 || encoded.size()>511)
                    throw std::runtime_error("Invalid simulation text encoding");
                auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;};
                for(size_t i=1;i<encoded.size();i+=2) {
                    const auto a=digit(encoded[i]),b=digit(encoded[i+1]);
                    if(a<0 || b<0)throw std::runtime_error("Invalid simulation text encoding");
                    edit.text+=static_cast<char>((a<<4)|b);
                }
            } else {value(input,edit.value);if(!std::isfinite(edit.value)) throw std::runtime_error("Invalid simulation setting number");}
            if(started) edit_setting(edit);
            else {
                if(initial_settings.size()>=1024) throw std::runtime_error("Too many initial settings");
                initial_settings.push_back(edit);
            }
        } else if(command=="keys") {
            provider.keys.clear();std::string name;
            while(input>>name) {const auto key=crml::action_key(name);if(!key) throw std::runtime_error("Unknown simulation key");provider.keys.insert(key);}
        } else if(command=="focus") boolean(input,provider.focused);
        else if(command=="fresh") boolean(input,provider.fresh);
        else if(command=="visibility_observation") {
            value(input,provider.visibility_observation);
            if(provider.visibility_observation>2) throw std::runtime_error("Invalid visibility observation");
        }
        else if(command=="emergency") boolean(input,provider.emergency);
        else if(command=="feedback_renderer") {boolean(input,feedback_renderer);if(!feedback_renderer)feedback_ack.clear();}
        else if(command=="ui_renderer") boolean(input,provider.ui_renderer);
        else if(command=="ui_new_page") {bool replace{};boolean(input,replace);if(replace && started)provider.new_ui_page();}
        else if(command=="ui_outcome") {
            value(input,provider.ui_outcome);
            if(provider.ui_outcome>3) throw std::runtime_error("Invalid UI outcome");
        }
        else if(command=="media_observer") boolean(input,provider.media_observer);
        else if(command=="media_consume") boolean(input,provider.media_consume);
        else if(command=="media") {
            value(input,provider.media_identity);boolean(input,provider.media_active);boolean(input,provider.media_ready);
            value(input,provider.media_elapsed);value(input,provider.media_source);
            if(!(input>>std::quoted(provider.media_name)) || provider.media_name.size()>255 ||
                provider.media_source!=0 && provider.media_source!=CRML_MEDIA_ENGINE_NAME && provider.media_source!=CRML_MEDIA_MAPPED_NAME)
                throw std::runtime_error("Invalid simulated media observation");
        }
        else if(command=="ui") {
            value(input,provider.ui_screen);value(input,provider.ui_actions);provider.ui_configured=true;
            if(provider.ui_screen>CRML_UI_SCREEN_MAIN_MENU || provider.ui_actions>1) throw std::runtime_error("Invalid UI observation");
        }
        else if(command=="heading") {
            result(input,provider.heading_result);for(auto& number:provider.heading) value(input,number);
        } else if(command=="player") {
            result(input,provider.player_result);provider.player.version=1;
            value(input,provider.player.generation);value(input,provider.player.age_ms);
            for(auto& number:provider.player.position) value(input,number);
        } else if(command=="navigation") {
            result(input,provider.navigation_result);provider.navigation={};provider.navigation.version=1;
            value(input,provider.navigation.generation);value(input,provider.navigation.sequence);
            value(input,provider.navigation.age_ms);value(input,provider.navigation.flags);
            for(auto& number:provider.navigation.position)value(input,number);
            double norm{};for(auto& number:provider.navigation.up){value(input,number);norm+=static_cast<double>(number)*number;}
            if(!provider.navigation.generation || !provider.navigation.sequence || provider.navigation.flags>15 ||
               ((provider.navigation.flags&CRML_NAV_UP_VALID) && (norm<0.98 || norm>1.02)))
                throw std::runtime_error("Invalid navigation observation");
            if(!(provider.navigation.flags&CRML_NAV_UP_VALID))for(auto& number:provider.navigation.up)number=0;
        } else if(command=="camera") {
            result(input,provider.camera_result);provider.camera.version=1;
            value(input,provider.camera.generation);value(input,provider.camera.age_ms);value(input,provider.camera.mode);value(input,provider.camera.flags);
            for(auto& number:provider.camera.position) value(input,number);
            for(auto& number:provider.camera.basis) value(input,number);
            value(input,provider.camera.horizontal_fov_radians);value(input,provider.camera.aspect_ratio);
        } else if(command=="physics") {
            result(input,provider.physics_result,-3);provider.physics.version=1;
            value(input,provider.physics.age_ms);value(input,provider.physics.flags);
            value(input,provider.physics.linear_damping);value(input,provider.physics.angular_damping);
            value(input,provider.physics.linear_speed);value(input,provider.physics.angular_speed);
        } else if(command=="status") {
            value(input,provider.status);if(provider.status<0 || provider.status>9) throw std::runtime_error("Invalid physics status");
        } else if(command=="return") {
            std::string name;value(input,name);
            if(name!="motion_set" && name!="visibility_set" && name!="physics_select" && name!="physics_apply" && name!="physics_restore")
                throw std::runtime_error("Unknown scripted operation");
            int code{};value(input,code);
            if(code==-99) provider.outcomes.erase(name);
            else if(code>=-3 && code<=-1) provider.outcomes[name]=code;
            else throw std::runtime_error("Scripted outcomes must be failures or -99 (default)");
        } else if(command=="start") {
            if(started) throw std::runtime_error("Simulation already started");
            feedback.enable_renderer((provider.caps&CRML_CAP_FEEDBACK)!=0);feedback_page=feedback.open_page();
            lists.enable_renderer(lists_renderer && (provider.caps&CRML_CAP_LISTS));
            provider.ui.enable((provider.caps&(CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|CRML_CAP_UI_PRESENTATION))!=0);
            provider.media.enable((provider.caps&(CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP))!=0);
            provider.observe_media();
            provider.new_ui_page();provider.render_ui();
            started=true;runtime.load(mods);
            if(!runtime.active() && !runtime.failures())
                throw std::runtime_error("No mod packages loaded; supply the directory containing package folders");
            for(const auto& edit:initial_settings) edit_setting(edit);
            initial_settings.clear();activate_lists();render_lists();render_feedback();provider.render_ui();provider.observe_media();provider.observe_visibility();
        } else if(command=="tick") {
            uint32_t milliseconds{};value(input,milliseconds);
            if(!started || ++provider.frame>10000 || milliseconds>60000) throw std::runtime_error("Invalid simulation tick");
            provider.now+=milliseconds;provider.expire();provider.render_ui();provider.observe_media();provider.observe_visibility();activate_lists();runtime.tick(float(milliseconds)/1000.f);
            render_lists();render_feedback();provider.render_ui();provider.observe_media();
        } else if(command=="finish") {
            if(!started) throw std::runtime_error("Simulation has not started");
            if(!list_actions.empty())throw std::runtime_error("Unapplied simulation list actions");
            runtime.shutdown();render_lists();finished=true;
            if(!lists.snapshot().empty())throw std::runtime_error("List ownership survived runtime shutdown");
            if(feedback_page) {
                std::vector<crml::ModFeedback::Message> messages;
                if(feedback.poll(feedback_page,++feedback_sequence,{},messages)!=200 || !messages.empty())
                    throw std::runtime_error("Feedback ownership survived runtime shutdown");
            }
        } else throw std::runtime_error("Unknown simulation command");
        if(command!="keys") {std::string extra;if(input>>extra) throw std::runtime_error("Extra simulation arguments");}
        if(provider.overflow) throw std::runtime_error("Simulation event limit exceeded");
    }
    if(!finished) throw std::runtime_error("Incomplete simulation input");
    if(profile) runtime.report_metrics();
    std::cout<<"@summary "<<runtime.failures()<<'\n';
    return runtime.failures()?1:0;
}
