#include "services/display/clock.h"
#include "ui/runtime/ui.h"
static struct ln_calendar draft;
static bool active,editing,pending;
static uint32_t edit_field;
static char labels[6][24],hint[96],notice[96];
static const char *source="Clock unavailable";
static bool same(const char *a,const char *b){if(!a||!b)return false;while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void copy(char *out,uint32_t capacity,const char *value){uint32_t at=0;while(value&&value[at]&&at+1u<capacity){out[at]=value[at];++at;}out[at]=0;}
static uint8_t month_days(uint16_t year,uint8_t month){
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(month<1u||month>12u)return 0;
    return (uint8_t)(days[month-1u]+(month==2u&&!(year&3u)));
}
static bool valid_calendar(const struct ln_calendar *value){
    return value&&value->year>=2000u&&value->year<=2099u&&value->month>=1u&&value->month<=12u&&
        value->day>=1u&&value->day<=month_days(value->year,value->month)&&value->hour<24u&&value->minute<60u&&value->second<60u;
}
static void label(uint32_t field,const char *name,uint32_t value,uint32_t digits){
    uint32_t at=0;while(*name&&at<15u)labels[field][at++]=*name++;
    labels[field][at++]=':';labels[field][at++]=' ';
    char reverse[4];uint32_t count=0;
    do{reverse[count++]=(char)('0'+value%10u);value/=10u;}while(value&&count<4u);
    while(count<digits)reverse[count++]='0';
    while(count)labels[field][at++]=reverse[--count];
    if(editing&&edit_field==field){labels[field][at++]=' ';labels[field][at++]='*';}
    labels[field][at]=0;
}
static void refresh_labels(void){
    label(0,"Year",draft.year,4);label(1,"Month",draft.month,2);label(2,"Day",draft.day,2);
    label(3,"Hour",ln_ui_clock_12()?(draft.hour%12u?draft.hour%12u:12u):draft.hour,2);
    if(ln_ui_clock_12()){uint32_t at=0;while(labels[3][at])++at;labels[3][at++]=' ';labels[3][at++]=draft.hour<12u?'A':'P';labels[3][at++]='M';labels[3][at]=0;}label(4,"Minute",draft.minute,2);
    copy(labels[5],sizeof(labels[5]),pending?"Saving...":"Save date and time");
}
void ln_clock_editor_begin(const struct ln_calendar *baseline,bool valid){
    active=true;editing=pending=false;edit_field=0;notice[0]=0;
    valid=valid&&valid_calendar(baseline);
    draft=valid?*baseline:(struct ln_calendar){.year=2000,.month=1,.day=1};
    source=valid?"Device clock":"Unverified draft";
    copy(hint,sizeof(hint),valid?"Select: Edit  Menu: Cancel":"Time unverified. Edit, then Save.");
    refresh_labels();
}
void ln_clock_editor_cancel(void){
    active=editing=pending=false;edit_field=0;draft=(struct ln_calendar){0};
    hint[0]=0;source="Clock unavailable";for(uint32_t i=0;i<6u;++i)labels[i][0]=0;
}
static uint32_t adjusted(uint32_t value,int32_t delta,uint32_t low,uint32_t high){
    if(delta>0){uint32_t step=(uint32_t)delta;return step>high-value?high:value+step;}
    if(delta<0){uint32_t step=0u-(uint32_t)delta;return step>value-low?low:value-step;}
    return value;
}
static bool adjust(int32_t delta){
    struct ln_calendar before=draft;
    if(edit_field==0u)draft.year=(uint16_t)adjusted(draft.year,delta,2000,2099);
    else if(edit_field==1u)draft.month=(uint8_t)adjusted(draft.month,delta,1,12);
    else if(edit_field==2u)draft.day=(uint8_t)adjusted(draft.day,delta,1,month_days(draft.year,draft.month));
    else if(edit_field==3u)draft.hour=(uint8_t)adjusted(draft.hour,delta,0,23);
    else if(edit_field==4u)draft.minute=(uint8_t)adjusted(draft.minute,delta,0,59);
    uint8_t maximum=month_days(draft.year,draft.month);if(draft.day>maximum)draft.day=maximum;
    if(before.year==draft.year&&before.month==draft.month&&before.day==draft.day&&
       before.hour==draft.hour&&before.minute==draft.minute)return false;
    source="Unsaved changes";refresh_labels();return true;
}
int ln_clock_editor_input(struct ln_ui_state *state,uint32_t kind,int32_t value){
    if(!active||!state||state->scene>=ln_ui_scene_count||
       !same(ln_ui_scenes[state->scene].id,"linnet.system.clock-edit")||(kind!=1u&&kind!=2u))return false;
    if(state->hold)return false;
    if(kind==1u){
        if(!pending&&!editing)return false;
        if(!pending&&value&&adjust(value)){state->dirty|=1u;return 1;}
        return 2;
    }
    uint32_t pressed=(uint32_t)value&~state->buttons;
    /* An accepted save owns the RTC until delayed verification completes. */
    if(pending){state->buttons=(uint32_t)value;return true;}
    /* Menu navigation and cancellation belong to the caller's common bridge. */
    if(pressed&1u)return false;
    if(!(pressed&16u)){
        if(editing){state->buttons=(uint32_t)value;return true;}
        return false;
    }
    state->buttons=(uint32_t)value;
    if(state->selected>5u)state->selected=5u;
    if(editing){editing=false;state->selected=edit_field+1u;copy(hint,sizeof(hint),"Select: Edit  Menu: Cancel");}
    else if(state->selected<5u){editing=true;edit_field=state->selected;copy(hint,sizeof(hint),"Wheel: Change  Select: Done");}
    else {pending=true;copy(hint,sizeof(hint),"Saving date and time...");}
    if(state->scene<LINNET_UI_MAX_SCENES)state->selections[state->scene]=state->selected;
    refresh_labels();state->dirty|=1u;return true;
}
bool ln_clock_editor_pending(void){return active&&pending;}
bool ln_clock_editor_request(struct ln_calendar *out){
    if(!out||!ln_clock_editor_pending())return false;*out=draft;out->second=0;return true;
}
void ln_clock_editor_result(enum ln_calendar_result result){
    if(!active)return;pending=editing=false;
    if(result==LN_CALENDAR_SAVED){draft.second=0;source="Device clock";copy(hint,sizeof(hint),"Date and time saved.");}
    else if(result==LN_CALENDAR_RESTORED){source="Original time restored";copy(hint,sizeof(hint),"Save failed. Original time restored.");}
    else if(result==LN_CALENDAR_UNKNOWN){source="Clock state unknown";copy(hint,sizeof(hint),"Save failed. Clock state unknown.");}
    else if(result==LN_CALENDAR_INVALID)copy(hint,sizeof(hint),"Invalid date or time. Review the fields.");
    else copy(hint,sizeof(hint),"Clock unavailable. Try Save again.");
    copy(notice,sizeof(notice),hint);
    refresh_labels();
}
void ln_clock_editor_block(const char *reason){
    pending=false;copy(hint,sizeof(hint),reason?reason:"Clock unavailable. Try Save again.");
    copy(notice,sizeof(notice),hint);if(active)refresh_labels();
}
const char *ln_clock_editor_text(const struct ln_ui_node *node,uint32_t item,const char *fallback){
    if(!node)return fallback;
    if(same(node->text,"Clock source")&&notice[0])return notice;
    if(!active)return fallback;
    if(same(node->text,"Clock fields")&&item<6u)return labels[item];
    if(same(node->text,"Clock edit hint"))return hint;
    if(same(node->text,"Clock source"))return source;
    if(same(node->text,"Clock edit title")){
        static const char *const titles[]={"Edit year","Edit month","Edit day","Edit hour","Edit minute"};
        return editing?titles[edit_field]:"Set Date & Time";
    }
    return fallback;
}
