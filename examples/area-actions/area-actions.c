#include "crml.h"

// Policy belongs to this guest. The host never selects a mod's actions or
// silently enables the example on behalf of the player.
static int handles[6];
static int ready;
static int previous_result = -99;
static float elapsed;
uint32_t crml_abi_version(void) {return 1;}
void crml_init(void) {
    const crml_setting_definition definitions[6] = {
        {1,CRML_SETTING_BOOL,"enabled","Allow actions in story areas",
         "Keeps conversation, quest and other restrictions. Does not unlock abilities.",0,0,1,1},
        {1,CRML_SETTING_BOOL,"melee","Melee","Request an area exception for melee attacks.",1,0,1,1},
        {1,CRML_SETTING_BOOL,"dodge","Dodge","Request an area exception for dodge.",1,0,1,1},
        {1,CRML_SETTING_BOOL,"jump","Jump","Request an area exception for jump.",1,0,1,1},
        {1,CRML_SETTING_BOOL,"parkour","Parkour","Request an area exception for parkour.",1,0,1,1},
        {1,CRML_SETTING_BOOL,"dash","Dash","Request an area exception for dash.",1,0,1,1}
    };
    ready=1;
    for(unsigned i=0;i<6;++i) {
        handles[i]=crml_settings_register(&definitions[i],sizeof(definitions[i]));
        if(handles[i]<=0) ready=0;
    }
}
void crml_tick(float dt) {
    elapsed+=dt;
    if(elapsed<0.1f) return;
    elapsed=0;
    crml_setting_value values[6];
    uint32_t actions=0;
    if(ready && crml_settings_read(values,sizeof(values))==6) {
        int valid=1;
        for(unsigned i=0;i<6;++i) if(values[i].handle!=(uint32_t)handles[i]) valid=0;
        if(valid && values[0].value!=0)
            for(unsigned i=1;i<6;++i) if(values[i].value!=0) actions|=1u<<(i-1);
    }
    const int result=crml_action_rule_set(actions,actions?CRML_RULE_RESTRICTION_AREA:0);
    if(result!=previous_result) {
        const char* message;
        if(result==1) message="Area exception requested; other native rules still apply.";
        else if(result==0) message="Area exception released.";
        else if(result==-5) message="Area exception waiting for a player.";
        else message="Area exception unavailable.";
        unsigned length=0;while(message[length]) ++length;
        crml_log(result<0?2:1,message,length);
        previous_result=result;
    }
}
void crml_shutdown(void) {crml_action_rule_set(0,0);}
