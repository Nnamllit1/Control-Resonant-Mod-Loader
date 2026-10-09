#include "crml.h"

/* Local example data, not extracted game content. Filtering, paging and selection
   are guest decisions; the host only publishes copied rows and queues clicks. */
static const char* entries[]={"Amber","Ash","Birch","Blue","Bronze","Cedar","Cloud","Copper","Coral","Dawn","Elm","Fern","Flint","Gold","Green","Hazel","Indigo","Iris","Ivory","Jade","Juniper","Lake","Laurel","Lilac","Maple","Mist","Moss","Oak","Olive","Pearl","Pine","Quartz","Reed","Rose","Ruby","Sage","Silver","Slate","Teal","Willow"};
enum { PAGE_SIZE=8, PREVIOUS=1001, NEXT=1002 };
static uint32_t search_handle,font_handle,selected,page_index;
static uint64_t search_revision,font_revision,published_revision,next_publish;
static int ready,dirty=1,last_failure;
static char query[64];
static float font_scale=1;
static void copy(char* out,const char* in,unsigned capacity){unsigned i=0;for(;i+1<capacity&&in[i];++i)out[i]=in[i];out[i]=0;}
static char lower(char c){return c>='A'&&c<='Z'?(char)(c+'a'-'A'):c;}
static int matches(const char* value) {
    if(!query[0])return 1;
    for(unsigned start=0;value[start];++start){unsigned n=0;while(query[n]&&value[start+n]&&lower(query[n])==lower(value[start+n]))++n;if(!query[n])return 1;}
    return 0;
}
static void publish(void) {
    uint32_t found[40],count=0;for(unsigned i=0;i<40;++i)if(matches(entries[i]))found[count++]=i;
    if(page_index*PAGE_SIZE>=count)page_index=0;
    crml_list_page page={0};page.version=1;page.size=sizeof(page);page.font_scale=font_scale;
    copy(page.title,"Example catalog",sizeof(page.title));
    for(unsigned i=page_index*PAGE_SIZE;i<count&&i<(page_index+1)*PAGE_SIZE;++i) {
        crml_list_row* row=&page.rows[page.row_count++];row->id=found[i]+1;
        row->flags=CRML_LIST_ROW_ENABLED|(row->id==selected?CRML_LIST_ROW_SELECTED:0);
        copy(row->label,entries[found[i]],sizeof(row->label));copy(row->detail,"Choose this example entry.",sizeof(row->detail));
    }
    crml_list_row* row=&page.rows[page.row_count++];row->id=PREVIOUS;row->flags=page_index?CRML_LIST_ROW_ENABLED:0;
    copy(row->label,"Previous entries",sizeof(row->label));
    row=&page.rows[page.row_count++];row->id=NEXT;row->flags=(page_index+1)*PAGE_SIZE<count?CRML_LIST_ROW_ENABLED:0;
    copy(row->label,"Next entries",sizeof(row->label));
    const int64_t result=crml_list_publish(&page,sizeof(page));
    if(result>0){published_revision=(uint64_t)result;dirty=0;last_failure=0;}
    else if(result!=-4&&result!=last_failure){last_failure=(int)result;crml_log(2,"List renderer unavailable or page rejected; retrying.",sizeof("List renderer unavailable or page rejected; retrying.")-1);}
}
uint32_t crml_abi_version(void){return 1;}
void crml_init(void) {
    const crml_text_setting_definition search={1,63,"search","Search","Case-insensitive ASCII search across all 40 example entries.",""};
    const crml_setting_definition font={1,CRML_SETTING_NUMBER,"font_scale","List text size","Scale the list text; leaves normal settings controls unchanged.",1,.75,1.5,.25};
    const int first=crml_settings_text_register(&search,sizeof(search)),second=crml_settings_register(&font,sizeof(font));
    ready=first>0&&second>0;search_handle=(uint32_t)first;font_handle=(uint32_t)second;
}
void crml_tick(float dt) {
    (void)dt;if(!ready)return;
    const uint64_t now=crml_clock_ms();crml_setting_value settings[2];crml_text_setting_value search;
    if(crml_settings_read(settings,sizeof(settings))!=2||settings[0].handle!=search_handle||settings[1].handle!=font_handle||
       crml_settings_text_read(search_handle,&search,sizeof(search))!=1)return;
    const int changed=search.revision!=search_revision||settings[1].revision!=font_revision;
    if(changed){copy(query,search.value,sizeof(query));page_index=0;font_scale=(float)settings[1].value;search_revision=search.revision;font_revision=settings[1].revision;dirty=1;}
    for(unsigned i=0;i<4;++i) {
        crml_list_event event;const int result=crml_list_next(&event,sizeof(event));
        if(result!=1){if(result<0)dirty=1;break;}
        // A settings edit changes which rows should be active. Drop actions from
        // that prior view as well as accepted actions for a replaced revision.
        if(changed||event.revision!=published_revision)continue;
        if(event.row_id==PREVIOUS){if(page_index)--page_index;dirty=1;}
        else if(event.row_id==NEXT){++page_index;dirty=1;}
        else if(event.row_id>=1&&event.row_id<=40){selected=(uint32_t)event.row_id;dirty=1;crml_log(1,"Example list selection changed.",sizeof("Example list selection changed.")-1);}
    }
    if(dirty&&now>=next_publish){next_publish=now+200;publish();}
}
void crml_shutdown(void){crml_list_hide();}
