#include "preferences.h"
#include "linnet_scene_ids.h"
enum { DISPLAY, BRIGHTNESS, GROUPS, ITEMS, ACTIONS };
static uint32_t page,group,chosen,chosen_row,level=100;
/* The editor has at most sixteen stable IDs. Build its row list on entry or
 * an accepted change, never while resolving individual rendered labels. */
static uint8_t rows[16],row_count;
static uint32_t shown_mask,rows_revision,rows_group=UINT32_MAX;
static char label[96];
static const char *const groups[]={"Home","Music","Settings","Extras"};
static bool equal(const char*a,const char*b){if(!a||!b)return false;while(*a&&*a==*b){++a;++b;}return *a==*b;}
__attribute__((weak)) void ln_preferences_changed(void){}
__attribute__((weak)) const char *ln_preferences_status(void){return "Preview";}
__attribute__((weak)) bool ln_preferences_editable(void){return true;}
__attribute__((weak)) bool ln_preferences_timeout_adjust(int32_t direction){(void)direction;return false;}
__attribute__((weak)) const char *ln_preferences_timeout_label(void){return "Backlight: 30 sec";}
uint32_t ln_preferences_brightness(void){return level;}
void ln_preferences_restore_brightness(uint32_t value){if(value>=5u&&value<=100u)level=value;}
static bool display_node(const struct ln_ui_node*n){
    const struct ln_ui_scene *scene=&ln_ui_scenes[LN_UI_SCENE_DISPLAY];
    for(uint32_t j=0;j<scene->count;++j)if(n==&scene->nodes[j])return true;
    return false;
}
static void prepare_rows(void){
    uint32_t revision=ln_ui_menu_revision();
    if(rows_group==group&&rows_revision==revision)return;
    row_count=0;shown_mask=0;
    for(uint32_t i=0;i<ln_ui_menu_count(group);++i){uint32_t id=ln_ui_menu_item(group,i);
        shown_mask|=1u<<id;if(ln_ui_menu_available(group,id))rows[row_count++]=(uint8_t)id;}
    for(uint32_t id=0;id<ln_ui_menu_limit(group);++id){
        if(!(shown_mask&(1u<<id))&&ln_ui_menu_available(group,id))rows[row_count++]=(uint8_t)id;
    }
    rows_group=group;rows_revision=revision;
}
static bool shown(uint32_t id){prepare_rows();return (shown_mask&(1u<<id))!=0;}
static uint32_t item_at(uint32_t row){prepare_rows();return row<row_count?rows[row]:UINT32_MAX;}
static uint32_t item_count(void){prepare_rows();return row_count;}
static const char *joined(const char*a,const char*b){uint32_t at=0;while(*a&&at<sizeof label-1u)label[at++]=*a++;while(*b&&at<sizeof label-1u)label[at++]=*b++;label[at]=0;return label;}
static const char *brightness_label(void){char digits[5];uint32_t n=level,at=0;digits[at++]=(char)('0'+n/100u);digits[at++]=(char)('0'+(n/10u)%10u);digits[at++]=(char)('0'+n%10u);digits[at++]='%';digits[at]=0;return joined("Brightness: ",digits+(level<10u?2u:level<100u?1u:0u));}
uint32_t ln_preferences_count(const struct ln_ui_node*n,uint32_t fallback){
    if(!display_node(n)||(n->kind!=LN_UI_VIRTUAL_LIST&&n->kind!=LN_UI_SCROLLBAR))return fallback;
    return page==DISPLAY?3u:page==BRIGHTNESS?1u:page==GROUPS?4u:page==ITEMS?item_count():4u;
}
const char *ln_preferences_text(const struct ln_ui_node*n,uint32_t item,const char*fallback){
    if(!display_node(n))return fallback;
    if(n->kind==LN_UI_STATUS_BAR)return page==DISPLAY?"Display":page==BRIGHTNESS?"Brightness":page==GROUPS?"Edit Menus":page==ITEMS?groups[group]:ln_ui_menu_label(group,chosen);
    if(n->kind==LN_UI_TEXT){
        if(item!=UINT32_MAX)return fallback;
        if(equal(n->text,"Display hint"))return page==BRIGHTNESS?"Wheel: adjust / Menu: back":page==ITEMS?"Select: edit / Menu: back":page==ACTIONS?"Select: apply / Menu: back":"Select: open / Menu: back";
        return ln_preferences_status();
    }
    if(n->kind!=LN_UI_VIRTUAL_LIST)return fallback;
    if(page==DISPLAY)return item==0?brightness_label():item==1?ln_preferences_timeout_label():"Themes";
    if(page==BRIGHTNESS)return brightness_label();
    if(page==GROUPS)return item<4?groups[item]:"";
    if(page==ITEMS){uint32_t id=item_at(item);if(id==UINT32_MAX)return "";return joined(shown(id)?"[x] ":"[ ] ",ln_ui_menu_label(group,id));}
    if(item==0)return ln_ui_menu_required_item(group,chosen)?"Always included":shown(chosen)?"Hide item":"Show item";
    return item==1?"Move up":item==2?"Move down":"Back";
}
void ln_preferences_open(struct ln_ui_state*s){if(ln_ui_show(s,"linnet.system.display")){page=GROUPS;s->selected=0;s->first_visible[s->scene]=0;ln_ui_refresh(s);}}
bool ln_preferences_input(struct ln_ui_state*s,uint32_t kind,int32_t value){
    if(!s||s->hold||s->scene!=LN_UI_SCENE_DISPLAY)return false;
    if(kind==1u&&page==BRIGHTNESS){if(!ln_preferences_editable())return true;int64_t next=(int64_t)level+(int64_t)value*5;if(next<5)next=5;if(next>100)next=100;
        if(level==(uint32_t)next)return false;level=(uint32_t)next;ln_preferences_changed();ln_ui_refresh(s);return true;}
    if(kind!=2u)return false;
    uint32_t pressed=(uint32_t)value&~s->buttons;s->buttons=(uint32_t)value;
    if(!pressed)return false;
    if(pressed&1u){if(page==DISPLAY||page==GROUPS){page=DISPLAY;(void)ln_ui_back(s);return true;}page=page==ACTIONS?ITEMS:page==ITEMS?GROUPS:DISPLAY;s->selected=page==ITEMS?chosen_row:0;}
    else if(pressed&(16u|4u|8u)){
        int32_t direction=(pressed&8u)?-1:1;
        if(page==DISPLAY){if(s->selected==0){page=BRIGHTNESS;s->selected=0;}else if(s->selected==1)(void)ln_preferences_timeout_adjust(direction);else (void)ln_ui_show(s,"linnet.system.themes");}
        else if(page==BRIGHTNESS){if(pressed&16u)page=DISPLAY;else if(ln_preferences_editable()){int32_t next=(int32_t)level+direction*5;if(next>=5&&next<=100){level=(uint32_t)next;ln_preferences_changed();}}}
        else if(page==GROUPS){group=s->selected;page=ITEMS;s->selected=0;}
        else if(page==ITEMS){chosen_row=s->selected;chosen=item_at(chosen_row);if(chosen!=UINT32_MAX){page=ACTIONS;s->selected=0;}}
        else {bool changed=false;if(ln_preferences_editable()){if(s->selected==0)changed=ln_ui_menu_toggle(group,chosen);else if(s->selected<3u)changed=ln_ui_menu_move(group,chosen,s->selected==1u?-1:1);}
            if(changed)ln_preferences_changed();page=ITEMS;s->selected=0;for(uint32_t i=0;i<item_count();++i)if(item_at(i)==chosen)s->selected=i;}
    }else return false;
    s->first_visible[s->scene]=0;ln_ui_refresh(s);return true;
}
