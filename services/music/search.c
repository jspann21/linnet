#include "search.h"
#include "library.h"
#include "browser.h"
#include "hal/api/platform.h"
#if !defined(LINNET_PROFILE_RELEASE)
static uint32_t query_started_us,first_ready_us,window_ready_us;
#endif
static const char alphabet[]="abcdefghijklmnopqrstuvwxyz0123456789 -'.";
struct cached_row { struct ln_music_library_search_row value; uint32_t ordinal; bool valid; };
static struct cached_row rows[LN_MUSIC_SEARCH_ROWS*2u];
static char prefix[LN_MUSIC_SEARCH_PREFIX+1],character[2],group_title[256],label[272];
static uint32_t length,first,known,total,revision,navigation,epoch,admitted_epoch,pending_ordinal,selected_track;
static uint32_t group_index,group_count,search_first,search_row,selected_ordinal,restore_row;
static bool restore_selection;
static bool opened,closing,results,pending,complete,track_ready,in_group,pending_group,window_ready,library_waiting;
static uint8_t result_identity[32],group_identity[32];
static uint32_t group_kind;
static bool result_identity_valid,group_identity_valid;
#if LINNET_TARGET_NUMBER == 25u
/* The native M25 targets have room for a retained parent window. */
static struct {
    struct ln_music_library_search_row rows[LN_MUSIC_SEARCH_ROWS];
    uint32_t first,count,known,total;
    uint8_t identity[32];char prefix[LN_MUSIC_SEARCH_PREFIX+1];
    bool valid,complete;
} parent;
static bool restore_cached;
static uint32_t restore_first;
static uint32_t find_deadline,find_epoch;
static bool find_deferred;
#endif
static struct cached_row *row_slot(uint32_t ordinal){return &rows[ordinal%(LN_MUSIC_SEARCH_ROWS*2u)];}
static bool row_ready(uint32_t ordinal){struct cached_row *r=row_slot(ordinal);return r->valid&&r->ordinal==ordinal;}
#if LINNET_TARGET_NUMBER == 25u
static bool parent_matches(void){
    if(!parent.valid||library_waiting||ln_music_library_status()!=LN_LIBRARY_READY)return false;
    uint8_t identity[32];if(!ln_music_library_identity(identity))return false;
    for(uint32_t i=0;i<32u;++i)if(identity[i]!=parent.identity[i])return false;
    for(uint32_t i=0;i<=LN_MUSIC_SEARCH_PREFIX;++i)if(prefix[i]!=parent.prefix[i])return false;
    return true;
}
static void parent_capture(uint32_t selected){
    parent.valid=false;
    if(in_group||!result_identity_valid||library_waiting||ln_music_library_status()!=LN_LIBRARY_READY)return;
    uint8_t identity[32];if(!ln_music_library_identity(identity))return;
    for(uint32_t i=0;i<32u;++i)if(identity[i]!=result_identity[i])return;
    uint32_t limit=first+LN_MUSIC_SEARCH_ROWS;if(complete&&limit>total)limit=total;
    if(selected<first||selected>=limit)return;
    uint32_t started=ln_time_us();
    for(uint32_t row=first;row<limit;++row){
        if((uint32_t)(ln_time_us()-started)>=250u||!row_ready(row))return;
        parent.rows[row-first]=row_slot(row)->value;
    }
    parent.first=first;parent.count=limit-first;parent.known=known;parent.total=total;parent.complete=complete;
    for(uint32_t i=0;i<32u;++i)parent.identity[i]=identity[i];
    for(uint32_t i=0;i<=LN_MUSIC_SEARCH_PREFIX;++i)parent.prefix[i]=prefix[i];
    parent.valid=true;
}

#endif
static void reset_results(uint32_t start){
    first=start;known=total=0;complete=window_ready=false;restore_selection=false;
#if LINNET_TARGET_NUMBER == 25u
    restore_cached=false;find_deferred=false;if(!in_group)parent.valid=false;
#endif
    if(!in_group)result_identity_valid=ln_music_library_identity(result_identity);
#if !defined(LINNET_PROFILE_RELEASE)
    query_started_us=results&&length?ln_time_us():0;
    first_ready_us=window_ready_us=0;
#endif
    for(uint32_t i=0;i<LN_MUSIC_SEARCH_ROWS*2u;++i)rows[i].valid=false;
    ++epoch;++revision;
}
void ln_music_search_open(void){
    opened=true;closing=false;results=false;track_ready=false;in_group=false;
    ln_music_browser_suspend(true);reset_results(0);++navigation;
}
void ln_music_search_close(void){opened=false;closing=true;track_ready=false;++epoch;++revision;}
#if LINNET_TARGET_NUMBER == 25u
void ln_music_search_cancel_frame_defer(void){find_deferred=false;}
bool ln_music_search_frame_deferred(uint32_t now,uint32_t *deadline){
    if(!find_deferred||!deadline)return false;
    if(!opened||closing||!results||!length||in_group||epoch!=find_epoch||window_ready||
       ln_music_library_status()!=LN_LIBRARY_READY||(int32_t)(now-find_deadline)>=0){
        find_deferred=false;return false;
    }
    *deadline=find_deadline;return true;
}
#endif
void ln_music_search_resume(void){
    if(opened)return;
    opened=true;closing=false;track_ready=false;++epoch;++revision;
    ln_music_browser_suspend(true);
}
bool ln_music_search_prepare(uint32_t visible_rows){
    if(!opened||closing||!results||!length||!in_group||!group_identity_valid||window_ready||
       ln_music_library_status()!=LN_LIBRARY_READY)return false;
    uint8_t identity[32];if(!ln_music_library_identity(identity))return false;
    for(uint32_t i=0;i<32u;++i)if(identity[i]!=group_identity[i])return false;
    struct ln_music_group group;
    if(!ln_music_library_cached_group(group_index,&group,0)||
       group.kind!=group_kind||group.count!=group_count)return false;
    bool filled=!complete||total!=group_count||known!=group_count;
    complete=true;total=known=group_count;
#if LINNET_TARGET_NUMBER==25u
    /* The existing retained window includes the next viewport. Resolve its
     * cached members here instead of opening asynchronous jobs for lookahead. */
    visible_rows=LN_MUSIC_SEARCH_ROWS;
#else
    if(visible_rows>LN_MUSIC_SEARCH_ROWS)visible_rows=LN_MUSIC_SEARCH_ROWS;
#endif
    uint32_t started=ln_time_us();
    for(uint32_t i=0;i<visible_rows&&first+i<group_count&&
        (uint32_t)(ln_time_us()-started)<250u;++i){
        uint32_t ordinal=first+i,track;
        if(row_ready(ordinal))continue;
#if LINNET_TARGET_NUMBER==25u
        if(!ln_music_library_cached_member_at(group.first+ordinal,&track))continue;
#else
        if(!ln_music_library_cached_member(group_index,ordinal,&track))continue;
#endif
        const char *title=ln_music_library_title(track);if(!title)continue;
        struct cached_row *cached=row_slot(ordinal);
        cached->value=(struct ln_music_library_search_row){.index=track};
        uint32_t n=0;while(n<255u&&title[n]){cached->value.title[n]=title[n];++n;}
        cached->value.title[n]=0;cached->ordinal=ordinal;cached->valid=true;filled=true;
    }
#if LINNET_TARGET_NUMBER==25u
    bool ready=true;
    for(uint32_t i=0;i<LN_MUSIC_SEARCH_ROWS&&first+i<group_count;i++)
        if(!row_ready(first+i)){ready=false;break;}
    if(ready&&!window_ready){window_ready=true;filled=true;}
#if !defined(LINNET_PROFILE_RELEASE)
    if(!first_ready_us&&first<group_count&&row_ready(first))first_ready_us=ln_time_us();
    if(window_ready&&!window_ready_us)window_ready_us=ln_time_us();
#endif
#endif
    if(filled)++revision;
    return filled;
}
void ln_music_search_step(void){
    if(!opened&&!closing)return;
    if(ln_music_library_status()!=LN_LIBRARY_READY){
        pending=false;complete=window_ready=library_waiting=true;total=known;
        if(closing){ln_music_browser_suspend(false);closing=false;}
        return;
    }
    if(library_waiting){library_waiting=false;reset_results(first);}
    if(pending){
        struct ln_music_library_search_row row;
        if(pending_group?!ln_music_library_take_view_row(&row):!ln_music_library_take_search_row(&row))return;
        pending=false;
        if(admitted_epoch==epoch&&opened&&results){
            if(row.index==UINT32_MAX){complete=true;total=pending_ordinal;}
            else {
                struct cached_row *cached=row_slot(pending_ordinal);
                cached->value=row;cached->ordinal=pending_ordinal;cached->valid=true;
                if(pending_ordinal>=known)known=pending_ordinal+1u;
            }
#if !defined(LINNET_PROFILE_RELEASE)
            if(!first_ready_us)first_ready_us=ln_time_us();
#endif
        }
    }
    if(closing){ln_music_browser_suspend(false);closing=false;return;}
    /* Viewport/result changes and library reload clear this published window. */
    if(window_ready||!results||!length||!ln_music_browser_query_idle())return;
#if LINNET_TARGET_NUMBER==25u
    /* Finish a budget-limited resident window in another bounded turn before
     * admitting a query. No cached progress keeps the asynchronous fallback. */
    if(!window_ready&&ln_music_search_prepare(LN_MUSIC_SEARCH_ROWS)&&!window_ready)return;
#endif
    uint32_t limit=first+LN_MUSIC_SEARCH_ROWS,ordinal=first;
    if(complete&&limit>total)limit=total;
    while(ordinal<limit&&row_ready(ordinal))++ordinal;
    bool ready=ordinal>=limit;
    /* Publish a complete visible window once; partial row redraws otherwise
     * steal time from the remaining disk reads and audio work. */
    if(ready&&!window_ready)++revision;
    window_ready=ready;
#if !defined(LINNET_PROFILE_RELEASE)
    if(window_ready&&!window_ready_us)window_ready_us=ln_time_us();
#endif
    if(window_ready)return;
    if(in_group?ln_music_library_view_row(2,group_index,ordinal):ln_music_library_search_at(prefix,ordinal)){
        pending_ordinal=ordinal;
        pending=true;pending_group=in_group;admitted_epoch=epoch;
    }
}
bool ln_music_search_busy(void){return closing||pending||(opened&&results&&length&&!window_ready);}
bool ln_music_search_query_pending(void){return pending;}
#if !defined(LINNET_PROFILE_RELEASE)
void ln_music_search_diagnostics(uint32_t out[16]){
    out[0]=0x48435253u;out[1]=0;out[2]=opened;out[3]=results;
    out[4]=ln_music_search_busy();out[5]=first;out[6]=known;out[7]=total;
    out[8]=revision;out[9]=query_started_us;out[10]=first_ready_us;out[11]=window_ready_us;
    out[12]=ln_time_us();out[13]=in_group;out[14]=complete;out[15]=window_ready;
}
#endif
bool ln_music_search_active(void){return opened||closing;}
uint32_t ln_music_search_count(void){
    if(!results)return sizeof alphabet;
    if(complete)return total?total:1u;
    return known+1u;
}
const char *ln_music_search_label(uint32_t row){
    if(!results){
        if(row==sizeof alphabet-1u)return "Show results >";
        if(row>=sizeof alphabet-1u)return "";
        if(alphabet[row]==' ')return "[space]";
        character[0]=alphabet[row];return character;
    }
    if(complete&&!total)return row?"":ln_music_library_status()!=LN_LIBRARY_READY?"Library unavailable":"No matches";
    if(row>=known)return !complete&&row==known?"Searching...":"";
    if(!row_ready(row))return "Loading...";
    const struct ln_music_library_search_row *result=&row_slot(row)->value;
    {
        if(!result->kind)return result->title;
        const char *tag=result->kind==1?"Artist: ":"Album: ";
        uint32_t n=0,i=0;
        while(tag[i])label[n++]=tag[i++];
        i=0;while(result->title[i])label[n++]=result->title[i++];label[n]=0;
        return label;
    }
}
const char *ln_music_search_prefix(void){return length?prefix:"Enter name prefix";}
const char *ln_music_search_hint(void){return results?(in_group?"Right: Actions  Menu: Results":"Right: Actions  Menu: Edit"):"Select: Add  Left: Erase  Right: Find";}
const char *ln_music_search_title(void){return in_group?group_title:results?"Search results":"Search music";}
uint32_t ln_music_search_revision(void){return revision;}
uint32_t ln_music_search_navigation(void){return navigation;}
void ln_music_search_right(void){if(opened&&length&&!results){
    results=true;reset_results(0);++navigation;
#if LINNET_TARGET_NUMBER == 25u
    /* Coalesce explicit Find into its ready result frame. This one-shot cap
     * never extends on progress and does not alter query scheduling. */
    find_deadline=ln_time_us()+50000u;find_epoch=epoch;find_deferred=true;
#endif
}}
void ln_music_search_left(void){
    if(!opened)return;
    if(results){(void)ln_music_search_back();return;}
    if(length){prefix[--length]=0;reset_results(0);}
}
bool ln_music_search_select(uint32_t row){
    if(!opened)return false;
    if(!results){
        if(row==sizeof alphabet-1u){ln_music_search_right();return length!=0;}
        if(row>=sizeof alphabet-1u||length==LN_MUSIC_SEARCH_PREFIX)return false;
        prefix[length++]=alphabet[row];prefix[length]=0;reset_results(0);return true;
    }
    if(row<known&&row_ready(row)){
        const struct ln_music_library_search_row *result=&row_slot(row)->value;
        if(result->kind){
#if LINNET_TARGET_NUMBER == 25u
            parent_capture(row);
#endif
            search_first=first;search_row=row;
            group_index=result->index&~LN_MUSIC_SEARCH_GROUP;group_count=result->count;in_group=true;
            group_kind=result->kind;group_identity_valid=result_identity_valid;
            for(uint32_t n=0;n<32u;++n)group_identity[n]=result_identity[n];
            uint32_t i=0;do{group_title[i]=result->title[i];}while(result->title[i++]&&i<256u);
            reset_results(0);++navigation;return true;
        }
        selected_track=result->index;selected_ordinal=row;track_ready=true;return true;
    }
    return false;
}
bool ln_music_search_back(void){
    if(in_group){
#if LINNET_TARGET_NUMBER == 25u
        bool retained=parent_matches();
#endif
        in_group=false;reset_results(search_first);
        restore_row=search_row;known=restore_row+1u;restore_selection=true;
#if LINNET_TARGET_NUMBER == 25u
        if(retained){
            first=parent.first;known=parent.known;total=parent.total;complete=parent.complete;window_ready=true;
            for(uint32_t i=0;i<parent.count;++i){struct cached_row *cached=row_slot(first+i);
                cached->value=parent.rows[i];cached->ordinal=first+i;cached->valid=true;
            }
            restore_first=first;restore_cached=true;
#if !defined(LINNET_PROFILE_RELEASE)
            first_ready_us=window_ready_us=ln_time_us();
#endif
        }
#endif
        ++navigation;return true;
    }
    if(results){results=false;reset_results(0);++navigation;return true;}
    ln_music_search_close();return false;
}
void ln_music_search_window(uint32_t first_visible){
    if(!results||first_visible==first)return;
    first=first_visible;window_ready=false;++epoch;++revision;
}
bool ln_music_search_take_selection(uint32_t *row){
    if(!row||!restore_selection)return false;
    *row=restore_row;restore_selection=false;return true;
}
bool ln_music_search_take_viewport(uint32_t *row,uint32_t *first){
#if LINNET_TARGET_NUMBER == 25u
    if(!row||!first||!restore_selection||!restore_cached)return false;
    *row=restore_row;*first=restore_first;restore_selection=restore_cached=false;return true;
#else
    (void)row;(void)first;return false;
#endif
}
bool ln_music_search_take_queue(struct ln_music_browser_queue *queue){
    if(!queue||!track_ready)return false;
    track_ready=false;*queue=(struct ln_music_browser_queue){0};
    if(!ln_music_library_identity(queue->library_hash))return false;
    if(in_group){
        queue->source=LN_QUEUE_GROUP;queue->source_group=group_index;
        queue->count=group_count;queue->cursor=selected_ordinal;
    }else {queue->count=1;queue->tracks[0]=selected_track;}
    return true;
}

bool ln_music_search_actions(uint32_t index){
    if(!opened||!results||index>=known||!row_ready(index))return false;
    const struct ln_music_library_search_row *r=&row_slot(index)->value;
    if(!ln_music_browser_external_actions(r->index,r->kind,r->count,r->title))return false;
    ln_music_search_close();return true;
}
