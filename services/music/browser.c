#include "browser.h"
#include "menu.h"
#include "ui/runtime/ui.h"
#include "library.h"
#include "collections.h"
#include "history.h"
#include "history_pages.h"
#include "hal/api/platform.h"

enum view { CATEGORIES,GROUPS,TRACKS,PLAYLISTS,LOCAL_TRACKS,ACTIONS,DESTINATION,NAME,CONFIRM,ACTIVITY,ARTIST_ALBUMS,ALBUM_ARTISTS,BROWSE_OPTIONS,LETTERS,MORE };
enum job { NONE,RANGE,GROUP,TITLE,MEMBER,RECORD,RESOLVE_IDS,EXPAND,ACTIVITY_IDS,FAVORITE_RECORD,BOOKMARK_RECORD,RECENT_GROUP,CATALOG_GROUP,CATALOG_MEMBER,CATALOG_ARTIST,CATALOG_RANGE,JUMP_GROUP,JUMP_TITLE,RECENT_IDS };
struct location {enum view view;uint32_t kind,group,count,lower,selected,slot,first;bool reverse,from_coverflow;char title[256];};
/* No published list ordinal reaches UINT32_MAX; it marks an invalid cache row. */
struct row {uint32_t ordinal,index,count;char label[256];};
/* Lists are bounded by published counts; each filter is cleared when its job starts. */
static struct {
    uint8_t filter[65536];uint32_t tracks[LINNET_QUEUE_SOURCE_MAX];
} activity_data __attribute__((section(".media_workspace"),aligned(32)));
static struct {
    struct location here,parents[8];uint32_t depth,first,visible,revision,navigation,epoch,job_epoch,job_row,lo,hi,mid,range;
    struct row rows[LN_MUSIC_BROWSER_ROWS*2];enum job job;
    bool coverflow,suspended,visible_now,was_ready,search,selection_ready,external,search_return;
    uint32_t restored_selection;
    struct ln_music_browser_queue queue;int status;uint32_t next;
    uint32_t action_row,action_index,action_count,action_group,action_kind;
    bool action_track,action_local,action_bookmark,bookmark_remove,recent_open;
    const char *notice;
    struct ln_collection draft;uint64_t activity_id;
    uint32_t activity_ordinal;
    uint32_t mapped[LINNET_QUEUE_CAPACITY],scan,resolve_count,sort_at,sort_end;
    /* Playlist resolution and activity ranking never run together. */
    union {uint32_t positions[LINNET_QUEUE_CAPACITY],activity_scores[LINNET_QUEUE_CAPACITY];};
    bool resolving,indexed,exporting,choosing,renaming,saved_message,empty_actions;uint32_t export_at,destination;
    struct ln_queue export_queue,expanded;bool expanding,expanded_ready;uint32_t expand_at;
    char name[32];uint32_t name_length;
    uint32_t activity_page,activity_count,activity_scan,activity_kind,scan_id_count,scan_id_at;
    uint32_t activity_recent_revision;
    uint8_t activity_recent_identity[32];
    bool activity_id_index_tried;
    uint64_t scan_ids[LN_MUSIC_ID_BATCH];
    uint32_t catalog,catalog_at,catalog_end,catalog_member,catalog_members,catalog_count,catalog_group;
    bool jumping,jump_binary;uint32_t jump_at,jump_lo,jump_hi,jump_found;char jump_letter;
    bool activity_scanning,activity_indexed,activity_waiting,activity_overflow,favorite_waiting,bookmark_waiting,activity_dirty;uint64_t favorite_id;
} b;
static const char *const categories[]={"Songs","Artists","Albums","Genres","Composers","Years","Folders","Playlists","Search","Favorites","Bookmarks","Recently Played","Most Played","Recently Added","Album Artists","Shuffle Songs"};
static void activity_insert(uint32_t track,uint32_t score){
    uint32_t at=b.activity_count;
    if(at==LINNET_QUEUE_CAPACITY){b.activity_overflow=true;at--;
        if(score<b.activity_scores[at]||(score==b.activity_scores[at]&&track>=activity_data.tracks[at]))return;}
    while(at&&(b.activity_scores[at-1]<score||
               (b.activity_scores[at-1]==score&&activity_data.tracks[at-1]>track))){
        activity_data.tracks[at]=activity_data.tracks[at-1];b.activity_scores[at]=b.activity_scores[at-1];at--;}
    activity_data.tracks[at]=track;b.activity_scores[at]=score;if(b.activity_count<LINNET_QUEUE_CAPACITY)b.activity_count++;
}
static const char alphabet[]=" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'";
static void copy(char *out,const char *in){uint32_t i=0;while(i<255&&in[i]){out[i]=in[i];i++;}out[i]=0;}
static bool activity_filter(uint64_t id,bool add){
    uint32_t x=(uint32_t)id^(uint32_t)(id>>32),y=(uint32_t)(id>>17)|1u;bool hit=true;
    for(uint32_t i=0;i<3;i++){uint32_t bit=(x+i*y)&524287u;if(add)activity_data.filter[bit/8]|=(uint8_t)(1u<<(bit&7));else if(!(activity_data.filter[bit/8]&(1u<<(bit&7))))hit=false;}return hit;
}
static void changed(void){b.revision++;}
static void invalidate(void){++b.epoch;for(uint32_t i=0;i<LN_MUSIC_BROWSER_ROWS*2;i++)b.rows[i].ordinal=UINT32_MAX;changed();}
static bool activity_empty(void){
    return !ln_music_history_error()&&
        ((b.activity_kind==10&&ln_paged_history_bookmark_count()==0)||
         (b.activity_kind==9&&ln_paged_history_favorites_empty())||
         (b.activity_kind==12&&ln_paged_history_played_empty()));
}
static bool recent_cached(void){
    if(!b.activity_scanning||b.activity_kind!=11u||b.job!=NONE||
       ln_music_library_status()!=LN_LIBRARY_READY||!ln_collections_ready())return false;
    uint32_t ignored;if(!ln_music_library_track_id_lookup(0,&ignored))return false;
    uint8_t identity[32];if(!ln_music_library_identity(identity))return false;
    uint32_t revision=ln_collections_revision();bool same=b.activity_indexed&&b.activity_recent_revision==revision;
    for(uint32_t i=0;i<32u&&same;++i)if(identity[i]!=b.activity_recent_identity[i])same=false;
    if(!same){
        b.activity_scan=b.activity_count=0;b.activity_recent_revision=revision;
        for(uint32_t i=0;i<32u;++i)b.activity_recent_identity[i]=identity[i];
    }
    b.activity_indexed=true;
    uint32_t total=ln_collections_recent_count(),started=ln_time_us();
    for(uint32_t n=0;n<64u&&b.activity_scan<total&&(uint32_t)(ln_time_us()-started)<250u;++n){
        uint64_t id;uint32_t ordinal;
        if(!ln_collections_recent_id(b.activity_scan,&id)||
           !ln_music_library_track_id_lookup(id,&ordinal)){b.activity_scan=b.activity_count=0;break;}
        if(ordinal!=UINT32_MAX)activity_insert(ordinal,total-b.activity_scan);
        ++b.activity_scan;
    }
    if(b.activity_scan==total){b.activity_scanning=false;b.here.count=b.activity_count;invalidate();}
    else changed();
    return true;
}
#if LINNET_TARGET_NUMBER==25u
static bool most_played_cached(void){
    if(!b.activity_scanning||b.activity_kind!=12u||b.job!=NONE||
       b.choosing||b.exporting||b.expanding||b.resolving||
       ln_music_library_status()!=LN_LIBRARY_READY||ln_music_history_error())return false;
    uint32_t began=ln_time_us(),count;
    /* This call never yields: the existing playlist scratch is consumed before
     * another foreground action can acquire it or change history/library. */
    if(!ln_paged_history_played_cached(b.draft.ids,b.mapped,16u,&count,began))return false;
    for(uint32_t n=0;n<count;++n){
        uint32_t track;
        if((uint32_t)(ln_time_us()-began)>=250u||
           !ln_music_library_track_id_lookup(b.draft.ids[n],&track))goto fallback;
        if(track!=UINT32_MAX)activity_insert(track,b.mapped[n]);
    }
    if((uint32_t)(ln_time_us()-began)>=250u)goto fallback;
    b.here.count=b.activity_count;b.activity_scanning=false;invalidate();return true;
fallback:
    b.activity_count=0;b.activity_overflow=false;return false;
}
#endif
static uint32_t ordinal(uint32_t n);
static struct row *row(uint32_t n){
    if(n==UINT32_MAX)return 0;
    struct row *r=&b.rows[n%(LN_MUSIC_BROWSER_ROWS*2)];
    if(r->ordinal==n)return r;
    if(n>=b.here.count)return 0;
    uint32_t at=ordinal(n),index=at,count=0;const char *label=0;
    if(b.here.view==GROUPS||b.here.view==ARTIST_ALBUMS||b.here.view==ALBUM_ARTISTS){
        if(b.here.view==ARTIST_ALBUMS&&!at)return 0;
        index=b.here.view==GROUPS?b.here.lower+at:activity_data.tracks[b.here.view==ARTIST_ALBUMS?at-1u:at];
        struct ln_music_group g;if(ln_music_library_cached_group(index,&g,&label))count=g.count;
    }else if(b.here.view==TRACKS){
        if(!b.here.kind||ln_music_library_cached_member(b.here.group,at,&index))label=ln_music_library_title(index);
    }else if(b.here.view==LOCAL_TRACKS||b.here.view==ACTIVITY){
        index=b.here.view==LOCAL_TRACKS?b.mapped[at]:activity_data.tracks[at];label=ln_music_library_title(index);
    }
    if(!label)return 0;
    *r=(struct row){.ordinal=n,.index=index,.count=count};copy(r->label,label);return r;}
static void navigate(void){b.navigation++;b.first=b.here.first;invalidate();b.selection_ready=true;b.restored_selection=b.here.selected;}
static uint32_t ordinal(uint32_t n){return b.here.reverse?(b.here.view==ARTIST_ALBUMS?(n?b.here.count-n:0):b.here.count-1-n):n;}
static void jump_label(const char *label){
    unsigned char letter=(unsigned char)label[0];if(letter>='a'&&letter<='z')letter-=32;if(letter<'A'||letter>'Z')letter='#';
    if(b.jump_binary){unsigned char key=(unsigned char)label[0];if(key>='A'&&key<='Z')key+=32;unsigned char target=(unsigned char)(b.jump_letter+32);if(letter==b.jump_letter)b.jump_found=b.jump_at;bool before=b.here.reverse?key>target:key<target;if(before)b.jump_lo=b.jump_at+1;else b.jump_hi=b.jump_at;}
    else if(letter==b.jump_letter){b.jumping=false;b.here.selected=b.jump_at;navigate();}else b.jump_at++;
}
static void jump_missing(void){
    /* The ordinary progress frame clamps selection to its only row. */
    b.jumping=false;b.here.selected=0;b.first=b.here.first=0;navigate();
    ln_music_browser_notice("No matching letter");
}
static bool jump_cached(void){
    if(b.resolving||ln_music_library_status()!=LN_LIBRARY_READY)return false;
    uint32_t started=ln_time_us();
    for(uint32_t step=0;step<64u&&b.jumping;++step){
        if(b.jump_binary){if(b.jump_lo==b.jump_hi){if(b.jump_found!=UINT32_MAX){b.jumping=false;b.here.selected=b.jump_found;navigate();}else jump_missing();return true;}b.jump_at=b.jump_lo+(b.jump_hi-b.jump_lo)/2;}
        else if(b.jump_at>=b.here.count){jump_missing();return true;}
        uint32_t at=ordinal(b.jump_at);
        if(b.here.view==ARTIST_ALBUMS&&!at){++b.jump_at;continue;}
        /* row() uses only verified resident labels and members. A miss keeps
         * the existing async lookup, including its cancellation epoch. */
        struct row *r=row(b.jump_at);if(!r)return false;
        jump_label(r->label);
        if((uint32_t)(ln_time_us()-started)>=250u)return true;
    }
    return true;
}
static bool list_view(void){return b.here.view==GROUPS||b.here.view==TRACKS||b.here.view==ACTIVITY||b.here.view==LOCAL_TRACKS||b.here.view==ARTIST_ALBUMS||b.here.view==ALBUM_ARTISTS;}
static bool push(void){if(b.depth==8)return false;b.parents[b.depth++]=b.here;return true;}
static void enter(enum view view,const char *title){b.here=(struct location){.view=view};copy(b.here.title,title);navigate();}
static uint32_t slots(void){uint32_t n=0;for(uint32_t i=0;i<LN_COLLECTION_SLOTS;i++){const char *name=ln_collection_name(i);if(name&&name[0])n++;}return n;}
static uint32_t slot_at(uint32_t n){for(uint32_t i=0;i<LN_COLLECTION_SLOTS;i++){const char *name=ln_collection_name(i);if(name&&name[0]){if(!n)return i;n--;}}return UINT32_MAX;}
void ln_music_browser_init(void){b.visible=6;b.visible_now=true;enter(CATEGORIES,"Music");}
void ln_music_browser_suspend(bool value){b.suspended=value;}
void ln_music_browser_set_visible(bool value){b.visible_now=value;}
bool ln_music_browser_window_needed(void){return false;}
bool ln_music_browser_query_idle(void){return b.job==NONE;}
#if LINNET_TARGET_NUMBER==25u && defined(LINNET_COVERFLOW_PACKED)
bool ln_music_browser_history_scan_visible(void){
    return b.visible_now&&!b.suspended&&b.here.view==ACTIVITY&&b.activity_scanning&&
        b.activity_page<LN_HISTORY_PAGE_COUNT&&b.activity_kind!=11u&&!ln_music_history_error();
}
#endif
bool ln_music_browser_take_coverflow(void){bool result=b.coverflow;b.coverflow=false;return result;}
bool ln_music_browser_take_search(void){bool result=b.search;b.search=false;return result;}
void ln_music_browser_publish(void){}
uint32_t ln_music_browser_revision(void){return b.revision+ln_collections_revision();}
uint32_t ln_music_browser_navigation(void){return b.navigation;}
bool ln_music_browser_take_selection(uint32_t *out,uint32_t *first){if(!out||!b.selection_ready)return false;*out=b.restored_selection;if(first)*first=b.here.first;b.selection_ready=false;return true;}
void ln_music_browser_notice(const char *text){b.notice=text;changed();}
uint32_t ln_music_browser_count(void){
    if(b.catalog||b.jumping||b.activity_scanning||b.resolving||b.exporting||b.expanding)return 1;
    switch(b.here.view){case CATEGORIES:case MORE:return ln_music_menu_count(b.here.view==MORE);case BROWSE_OPTIONS:return 3;case LETTERS:return 27;case PLAYLISTS:return slots()+2;case DESTINATION:return slots()+1;
    case NAME:return sizeof alphabet+1;case CONFIRM:return 2;
    case ACTIONS:return b.empty_actions?2:b.action_local?10:b.action_bookmark?6:b.action_track?5:4;
    default:return b.here.count?b.here.count:1;}
}
static const char *activity_progress(void){
    static char text[48];const char *label=categories[b.activity_kind];uint32_t n=0;while(label[n]&&n<28){text[n]=label[n];n++;}
    text[n++]=' ';uint32_t percent=b.activity_kind==11?0:b.activity_page*50u/LN_HISTORY_PAGE_COUNT;
    uint32_t total=ln_music_library_count();if(total)percent+=b.activity_scan*(b.activity_kind==11?100u:50u)/total;
    if(percent>=100)text[n++]='1';if(percent>=10)text[n++]=(char)('0'+percent/10%10);text[n++]=(char)('0'+percent%10);text[n++]='%';text[n]=0;return text;
}
const char *ln_music_browser_title(void){return b.notice?b.notice:b.catalog?"Finding albums...":b.jumping?"Finding letter...":b.activity_scanning?activity_progress():b.saved_message?ln_collections_status():b.expanding?"Preparing queue...":b.exporting?"Preparing playlist...":b.resolving?"Finding playlist tracks...":b.here.view==NAME?(b.name_length?b.name:"Enter playlist name"):b.here.title;}
const char *ln_music_browser_label(uint32_t n){
    if(b.catalog||b.jumping||b.activity_scanning||b.resolving||b.exporting)return "Menu: Cancel";if(b.expanding)return "Please wait";
    switch(b.here.view){
    case CATEGORIES:case MORE:{uint32_t id=ln_music_menu_category(b.here.view==MORE,n);return id<16?categories[id]:id==LN_MUSIC_COVER_FLOW?"Cover Flow":id==LN_MUSIC_MORE?"More":"";}
    case BROWSE_OPTIONS:{struct location *p=&b.parents[b.depth-1];bool alpha=p->view==GROUPS||p->view==ARTIST_ALBUMS||p->view==ALBUM_ARTISTS||(p->view==TRACKS&&!p->kind)||(p->view==ACTIVITY&&(b.activity_kind==9||b.activity_kind==10));return n==2?"Jump to letter...":alpha?(n?"Sort: Z to A":"Sort: A to Z"):(n?"Reverse track order":"Original track order");}
    case LETTERS:{static char letter[2];letter[0]=n?(char)('A'+n-1):'#';letter[1]=0;return letter;}
    case ARTIST_ALBUMS:if(!ordinal(n))return "All Songs";break;
    case PLAYLISTS:case DESTINATION:{uint32_t count=slots();if(n<count){const char *name=ln_collection_name(slot_at(n));return name?name:"";}
        if(!ln_collections_ready())return "Loading playlists...";return n==count?"New playlist...":"Synced playlists";}
    case NAME:{static char letter[2];if(!n)return "Save name";if(n==1)return "Erase last character";if(n-2>=sizeof alphabet-1)return "";letter[0]=alphabet[n-2];letter[1]=0;return letter[0]==' '?"[space]":letter;}
    case CONFIRM:return n?"Delete playlist":"Cancel";
    case ACTIONS:{if(b.action_bookmark&&(!n||n==5))return n?"Remove bookmark":"Resume bookmark";static const char *const actions[]={"Play","Play Next","Add to Queue","Add to Playlist...","Toggle Favorite","Move up","Move down","Remove track","Rename playlist...","Delete playlist..."};return b.empty_actions?(n?"Delete playlist...":"Rename playlist..."):n<(b.action_local?10u:b.action_track?5u:4u)?actions[n]:"";}
    default:if(!b.here.count)return b.range?"Loading...":b.here.view==ACTIVITY&&b.activity_kind==10?"No bookmarks":"No tracks";struct row *r=row(n);return r?r->label:"Loading...";
    }
    struct row *r=row(n);return r?r->label:"Loading...";
}
void ln_music_browser_visible_rows(uint32_t n){b.visible=n?n:1;if(b.visible>LN_MUSIC_BROWSER_ROWS)b.visible=LN_MUSIC_BROWSER_ROWS;}
void ln_music_browser_window(uint32_t n){b.first=b.here.first=n;}
static void groups(uint32_t kind){enter(GROUPS,kind==8?"Recently Added":categories[kind]);b.here.kind=kind;if(ln_music_library_group_range(kind,&b.here.lower,&b.here.count)){b.range=0;return;}b.range=1;b.lo=0;b.hi=ln_music_library_group_count();}
static void name_begin(bool rename){
    b.renaming=rename;b.name_length=0;
    const char *name=rename?b.draft.name:"";while(name[b.name_length]&&b.name_length<31){b.name[b.name_length]=name[b.name_length];b.name_length++;}b.name[b.name_length]=0;
    enter(NAME,"Playlist name");
}
static bool store_draft(uint32_t slot){
    if(!ln_collection_put(slot,&b.draft)){ln_music_browser_notice(ln_collections_status());return false;}
    b.choosing=false;b.external=false;b.exporting=false;b.depth=0;enter(CATEGORIES,"Music");b.here.selected=0;for(uint32_t i=0;i<ln_music_menu_count(false);++i)if(ln_music_menu_category(false,i)==7)b.here.selected=i;push();enter(PLAYLISTS,"Playlists");b.saved_message=true;return true;
}
static void resolve_prepare(bool indexed){
    b.indexed=indexed;b.scan=b.resolve_count=b.scan_id_count=b.scan_id_at=0;
    bool sorted=true;for(uint32_t i=0;i<b.draft.count;i++){b.mapped[i]=UINT32_MAX;b.positions[i]=i;if(i&&b.draft.ids[i]<b.draft.ids[i-1u])sorted=false;}
    b.sort_end=b.indexed||sorted||b.draft.count<=8u?0:b.draft.count;b.sort_at=b.sort_end/2u;
}
static void resolve_begin(uint32_t slot){
    const struct ln_collection *p=ln_collection_get(slot);if(!p)return;b.draft=*p;
    enter(LOCAL_TRACKS,b.draft.name);b.here.slot=slot;b.here.count=b.draft.count;b.resolving=b.draft.count!=0;
    uint32_t ignored;resolve_prepare(ln_music_library_track_id_lookup(0,&ignored));
}
static void resolve_complete(void){
    uint32_t count=0;
    for(uint32_t i=0;i<b.draft.count;i++)if(b.mapped[i]!=UINT32_MAX){b.positions[count]=i;b.mapped[count++]=b.mapped[i];}
    b.here.count=count;b.resolving=false;
    if(count!=b.draft.count)ln_music_browser_notice("Missing tracks omitted");
    changed();
}
/* Sort positions under the browser's existing metadata budget, one heap sift
 * per step. The draft and final playlist order remain unchanged. */
static void resolve_sift(uint32_t root,uint32_t count){
    uint32_t position=b.positions[root];uint64_t id=b.draft.ids[position];
    while(root<count/2u){uint32_t child=root*2u+1u;
        if(child+1u<count&&b.draft.ids[b.positions[child]]<b.draft.ids[b.positions[child+1u]])child++;
        if(id>=b.draft.ids[b.positions[child]])break;
        b.positions[root]=b.positions[child];root=child;
    }
    b.positions[root]=position;
}
static void resolve_id(uint64_t id){
    if(b.draft.count<=8u){for(uint32_t i=0;i<b.draft.count;i++)if(b.draft.ids[i]==id&&b.mapped[i]==UINT32_MAX){b.mapped[i]=b.scan;b.resolve_count++;}return;}
    uint32_t lo=0,hi=b.draft.count;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2u;if(b.draft.ids[b.positions[mid]]<id)lo=mid+1u;else hi=mid;}
    if(lo==b.draft.count||b.draft.ids[b.positions[lo]]!=id||b.mapped[b.positions[lo]]!=UINT32_MAX)return;
    for(uint32_t i=lo;i<b.draft.count&&b.draft.ids[b.positions[i]]==id;i++){
        uint32_t position=b.positions[i];if(b.mapped[position]==UINT32_MAX){b.mapped[position]=b.scan;b.resolve_count++;}
    }
}
static bool queue_cached(void){
    if(b.status!=LN_BROWSER_QUEUE_LOADING||b.queue.source!=LN_QUEUE_GROUP||
       b.job!=NONE||ln_music_library_status()!=LN_LIBRARY_READY)return false;
    uint32_t started=ln_time_us();
    for(uint32_t step=0;step<64u&&b.next<b.queue.count&&
        (uint32_t)(ln_time_us()-started)<250u;++step){
        uint32_t ordinal=b.queue.source_group&LN_QUEUE_REVERSE?b.queue.count-1u-b.next:b.next,track;
        if(!ln_music_library_cached_member(b.queue.source_group&~LN_QUEUE_REVERSE,ordinal,&track))return false;
        b.queue.tracks[b.next++]=track;
    }
    if(b.next==b.queue.count){b.queue.source=0;b.status=LN_BROWSER_QUEUE_READY;changed();}
    return true;
}
static void queue_begin(uint32_t count,uint32_t cursor,uint32_t source,uint32_t group,uint32_t action){
    b.queue=(struct ln_music_browser_queue){.count=count,.cursor=cursor,.source=source,.source_group=group,.action=action};
    if(!count||!ln_music_library_identity(b.queue.library_hash)){ln_music_browser_notice("No tracks to add");return;}
    if(action&&count>LINNET_QUEUE_CAPACITY){ln_music_browser_notice("Edit limit: 1024 tracks");return;}
    if(count>LINNET_QUEUE_CAPACITY||(!action&&source&&count>64&&!ln_collections_writable())){b.status=LN_BROWSER_QUEUE_READY;return;}
    b.next=0;b.status=LN_BROWSER_QUEUE_LOADING;b.notice="Preparing...";
    if(source==LN_QUEUE_ALL){for(uint32_t i=0;i<count;i++)b.queue.tracks[i]=group&LN_QUEUE_REVERSE?count-1-i:i;b.queue.source=0;b.status=LN_BROWSER_QUEUE_READY;}
    else (void)queue_cached();
    changed();
}
bool ln_music_browser_shuffle_all(void){
    if(ln_music_library_status()!=LN_LIBRARY_READY){ln_music_browser_notice("Music library not ready");return false;}
    uint32_t count=ln_music_library_count();
    if(!count){ln_music_browser_notice("No songs to shuffle");return false;}
    if(count>LINNET_QUEUE_SOURCE_MAX){ln_music_browser_notice("Library exceeds queue limit");return false;}
    if(b.exporting||b.expanding){ln_music_browser_notice("Queue busy; try again");return false;}
    if(b.status==LN_BROWSER_QUEUE_LOADING||(!b.bookmark_remove&&(b.bookmark_waiting||b.job==BOOKMARK_RECORD))){
        b.bookmark_waiting=false;invalidate();
    }
    queue_begin(count,0,LN_QUEUE_ALL,0,0);
    b.queue.shuffle_all=true;changed();return b.status==LN_BROWSER_QUEUE_READY;
}
bool ln_music_browser_actions(uint32_t selected){
    if(b.catalog||b.jumping||b.range||b.activity_scanning||b.resolving||b.exporting||b.expanding||b.status==LN_BROWSER_QUEUE_LOADING)return false;
    if(b.here.view!=ACTIVITY&&b.here.view!=TRACKS&&b.here.view!=GROUPS&&b.here.view!=LOCAL_TRACKS&&b.here.view!=PLAYLISTS&&b.here.view!=ARTIST_ALBUMS&&b.here.view!=ALBUM_ARTISTS)return false;
    b.saved_message=false;b.empty_actions=false;b.action_bookmark=b.here.view==ACTIVITY&&b.activity_kind==10;b.here.selected=selected;b.action_row=b.here.count?ordinal(selected):0;b.action_local=b.here.view==LOCAL_TRACKS;
    if(b.here.view==PLAYLISTS){uint32_t slot=slot_at(selected);if(slot==UINT32_MAX)return false;b.here.slot=slot;b.action_local=b.empty_actions=true;if(!push())return false;enter(ACTIONS,"Playlist actions");return true;}
    struct row *r=row(selected);
    if(!b.here.count&&b.action_local){b.empty_actions=true;if(!push())return false;enter(ACTIONS,"Playlist actions");return true;}
    if(!r){ln_music_browser_notice("Wait for track to load");return false;}
    b.action_track=b.here.view!=GROUPS&&b.here.view!=ARTIST_ALBUMS&&b.here.view!=ALBUM_ARTISTS;b.action_index=r->index;b.action_count=r->count;b.action_group=r->index;b.action_kind=b.here.kind;
    if(!push())return false;enter(ACTIONS,r->label);return true;
}
static void catalog_begin(bool owners){
    if(ln_music_library_albums(owners?UINT32_MAX:b.here.group,activity_data.tracks,LINNET_QUEUE_SOURCE_MAX,&b.catalog_count)){
        b.catalog=0;b.here.count=b.catalog_count+(owners?0u:1u);invalidate();return;
    }
    __builtin_memset(activity_data.filter,0,sizeof activity_data.filter);
    b.catalog=owners?4:1;b.lo=0;b.hi=ln_music_library_group_count();b.catalog_at=b.catalog_member=b.catalog_count=0;b.catalog_end=ln_music_library_group_count();
    b.here.count=0;invalidate();
}
bool ln_music_browser_options(uint32_t selected){
    if(!list_view()||b.catalog||b.jumping||b.range||b.activity_scanning||b.resolving||b.exporting||b.expanding||b.status==LN_BROWSER_QUEUE_LOADING)return false;
    b.here.selected=selected;if(!push())return false;enter(BROWSE_OPTIONS,"Browse options");return true;
}
bool ln_music_browser_row_navigates(uint32_t n){
    if(b.here.view==CATEGORIES||b.here.view==MORE)return ln_music_menu_category(b.here.view==MORE,n)!=15u;
    return b.here.view==GROUPS||b.here.view==PLAYLISTS||
        b.here.view==ARTIST_ALBUMS||b.here.view==ALBUM_ARTISTS;
}
bool ln_music_browser_select(uint32_t n){
    if(b.here.view==CATEGORIES&&ln_music_menu_category(false,n)==15u)return ln_music_browser_shuffle_all();
    if(b.catalog||b.jumping||b.range||b.activity_scanning||b.resolving||b.exporting||b.expanding||b.status==LN_BROWSER_QUEUE_LOADING)return false;
    b.notice=0;b.saved_message=false;b.here.selected=n;
    if(b.here.view==BROWSE_OPTIONS){if(n<2){struct location *parent=&b.parents[b.depth-1];if(parent->reverse!=(n!=0)&&parent->count)parent->selected=parent->view==ARTIST_ALBUMS?(parent->selected?parent->count-parent->selected:0):parent->count-1-parent->selected;parent->reverse=n!=0;return ln_music_browser_back();}if(n==2){enter(LETTERS,"Jump to letter");return true;}return false;}
    if(b.here.view==LETTERS){if(n>26)return false;char letter=n?(char)('A'+n-1):'#';ln_music_browser_back();b.jump_letter=letter;b.jumping=true;b.jump_lo=0;b.jump_hi=b.here.count;b.jump_at=0;b.jump_found=UINT32_MAX;
        /* The async one-row progress view resets the viewport origin. Keep
         * that placement when a cached jump finishes before its first frame. */
        b.first=b.here.first=0;
        b.jump_binary=n&&(b.here.view==GROUPS||b.here.view==ALBUM_ARTISTS||(b.here.view==TRACKS&&!b.here.kind)||(b.here.view==ACTIVITY&&(b.activity_kind==9||b.activity_kind==10)));(void)jump_cached();changed();return true;}
    if(b.here.view==CATEGORIES||b.here.view==MORE){n=ln_music_menu_category(b.here.view==MORE,n);if(n==LN_MUSIC_COVER_FLOW){b.coverflow=true;return true;}if(n==LN_MUSIC_MORE){if(!push())return false;enter(MORE,"More Music");return true;}if(n>=15)return false;if(n==14){if(!push())return false;enter(ALBUM_ARTISTS,"Album Artists");catalog_begin(true);return true;}if(n>=9&&n<=12){if(!push())return false;enter(ACTIVITY,categories[n]);b.activity_kind=n;b.activity_count=b.activity_scan=b.activity_page=b.scan_id_count=b.scan_id_at=0;b.activity_scanning=!activity_empty();b.activity_indexed=b.activity_waiting=b.activity_overflow=false;
        b.activity_id_index_tried=false;
#if LINNET_TARGET_NUMBER==25u
        if(most_played_cached())return true;
#endif
        if(n!=11u&&b.activity_scanning)__builtin_memset(activity_data.filter,0,sizeof activity_data.filter);
        (void)recent_cached();
        return true;}if(n==13){if(!push())return false;groups(8);copy(b.here.title,"Recently Added");return true;}if(n==8){b.search=true;return true;}if(!push())return false;
        if(n==7)enter(PLAYLISTS,"Playlists");else if(n)groups(n);else{enter(TRACKS,"Songs");b.here.count=ln_music_library_count();}return true;}
    if(b.here.view==PLAYLISTS||b.here.view==DESTINATION){
        uint32_t count=slots(),slot=slot_at(n);bool destination=b.here.view==DESTINATION;
        if(n==count){if(!ln_collections_writable()){ln_music_browser_notice(ln_collections_status());return false;}int free=ln_collection_free();if(free<0){ln_music_browser_notice("All 16 playlists are in use");return false;}b.destination=(uint32_t)free;if(!destination)b.draft=(struct ln_collection){0};push();name_begin(false);return true;}
        if(slot!=UINT32_MAX){if(destination){const struct ln_collection *p=ln_collection_get(slot);if(!p||p->count+b.draft.count>LINNET_QUEUE_CAPACITY){ln_music_browser_notice("Playlist limit: 1024 tracks");return false;}
                for(uint32_t i=b.draft.count;i;i--)b.draft.ids[p->count+i-1]=b.draft.ids[i-1];for(uint32_t i=0;i<p->count;i++)b.draft.ids[i]=p->ids[i];b.draft.count+=p->count;for(uint32_t i=0;i<32;i++)b.draft.name[i]=p->name[i];return store_draft(slot);}
            if(!push())return false;resolve_begin(slot);return true;}
        if(!destination&&n==count+1){if(!push())return false;groups(7);return true;}return false;
    }
    if(b.here.view==NAME){if(!n){if(!b.name_length){ln_music_browser_notice("Enter a playlist name");return false;}
            for(uint32_t i=0;i<LN_COLLECTION_SLOTS;i++){const char *name=ln_collection_name(i);if(i==b.destination||!name)continue;uint32_t j=0;while(b.name[j]&&b.name[j]==name[j])j++;if(!b.name[j]&&!name[j]){ln_music_browser_notice("Name already in use");return false;}}
            for(uint32_t i=0;i<32;i++)b.draft.name[i]=i<=b.name_length?b.name[i]:0;return store_draft(b.destination);}
        if(n==1){if(b.name_length)b.name[--b.name_length]=0;}else if(n-2<sizeof alphabet-1&&b.name_length<31){b.name[b.name_length++]=alphabet[n-2];b.name[b.name_length]=0;}changed();return true;}
    if(b.here.view==CONFIRM){if(!n)return ln_music_browser_back();b.draft=(struct ln_collection){0};return store_draft(b.destination);}
    if(b.here.view==ACTIONS){
        if(b.empty_actions)n+=8;
        if(b.action_bookmark&&(!n||n==5)){if(ln_music_library_track_record(b.action_index)){b.bookmark_remove=n==5;b.job=BOOKMARK_RECORD;b.job_epoch=b.epoch;return true;}ln_music_browser_notice("Please try again");return false;}
        if(n==4&&b.action_track){if(ln_music_library_track_record(b.action_index)){b.job=FAVORITE_RECORD;b.job_epoch=b.epoch;ln_music_browser_notice("Loading favorite...");return true;}ln_music_browser_notice("Please try again");return false;}
        if(n>=5&&b.action_local){struct location *parent=&b.parents[b.depth-1];uint32_t slot=parent->slot,index=b.positions[b.action_row];const struct ln_collection *p=ln_collection_get(slot);if(!p)return false;b.draft=*p;b.destination=slot;
            if(n==8){push();name_begin(true);return true;}if(n==9){push();enter(CONFIRM,"Delete this playlist?");return true;}
            if(index>=b.draft.count)return false;
            if(n==7){for(uint32_t i=index+1;i<b.draft.count;i++)b.draft.ids[i-1]=b.draft.ids[i];b.draft.count--;}
            else {bool up=(n==5)!=parent->reverse;uint32_t to=up?(index?index-1:index):(index+1<b.draft.count?index+1:index);uint64_t id=b.draft.ids[to];b.draft.ids[to]=b.draft.ids[index];b.draft.ids[index]=id;parent->selected=parent->reverse?parent->count-1-to:to;}
            if(!ln_collection_put(slot,&b.draft)){ln_music_browser_notice(ln_collections_status());return false;}ln_music_browser_back();return true;}
        if(n>3)return false;
        if(b.action_track){queue_begin(1,0,0,0,n);b.queue.tracks[0]=b.action_index;b.status=LN_BROWSER_QUEUE_READY;}
        else queue_begin(b.action_count,0,LN_QUEUE_GROUP,b.action_group,n);
        return true;
    }
    if(n>=b.here.count)return false;struct row *r=row(n);if(!r)return false;
    if(b.here.view==GROUPS||b.here.view==ALBUM_ARTISTS||b.here.view==ARTIST_ALBUMS){uint32_t group=r->index,count=r->count;bool artist=b.here.view==ALBUM_ARTISTS||(b.here.view==GROUPS&&b.here.kind==1);uint32_t kind=b.here.view==ARTIST_ALBUMS?(ordinal(n)?2:1):b.here.kind;char name[256];copy(name,r->label);if(!push())return false;enter(artist?ARTIST_ALBUMS:TRACKS,name);b.here.kind=kind;b.here.group=group;b.here.count=count;if(artist){b.here.slot=count;catalog_begin(false);}return true;}
    if(b.here.view==ACTIVITY){if(b.activity_kind==10){b.bookmark_remove=false;b.action_index=activity_data.tracks[ordinal(n)];if(ln_music_library_track_record(b.action_index)){b.job=BOOKMARK_RECORD;b.job_epoch=b.epoch;}return true;}
        if(b.here.count>LINNET_QUEUE_CAPACITY){queue_begin(1,0,0,0,0);b.queue.tracks[0]=activity_data.tracks[ordinal(n)];b.status=LN_BROWSER_QUEUE_READY;return true;}queue_begin(b.here.count,n,0,0,0);for(uint32_t i=0;i<b.here.count;i++)b.queue.tracks[i]=activity_data.tracks[ordinal(i)];b.status=LN_BROWSER_QUEUE_READY;return true;}
    if(b.here.view==LOCAL_TRACKS){queue_begin(b.here.count,n,0,0,0);for(uint32_t i=0;i<b.here.count;i++)b.queue.tracks[i]=b.mapped[ordinal(i)];b.status=LN_BROWSER_QUEUE_READY;return true;}
    queue_begin(b.here.count,n,b.here.kind?LN_QUEUE_GROUP:LN_QUEUE_ALL,b.here.group|(b.here.reverse?LN_QUEUE_REVERSE:0),0);return true;
}
bool ln_music_browser_select_single(uint32_t n){struct row *r=row(n);if(!r)return false;queue_begin(1,0,0,0,0);b.queue.tracks[0]=r->index;b.status=LN_BROWSER_QUEUE_READY;return true;}
bool ln_music_browser_open_album(uint32_t group,uint32_t count,const char *title){if(!count||!push())return false;enter(TRACKS,title);b.here.from_coverflow=true;b.here.kind=2;b.here.group=group;b.here.count=count;return true;}
bool ln_music_browser_back(void){
    if(b.expanding)return true;
    if(b.jumping){b.jumping=false;invalidate();return true;}
    bool rebuild_owners=b.here.view==ARTIST_ALBUMS;
    b.catalog=0;
    b.notice=0;b.saved_message=false;b.activity_scanning=b.activity_waiting=b.favorite_waiting=b.bookmark_waiting=false;b.exporting=b.resolving=false;b.status=LN_BROWSER_QUEUE_IDLE;b.range=0;b.recent_open=false;invalidate();
    if(b.external&&b.here.view==ACTIONS){b.external=false;b.search_return=true;}
    if(!b.depth)return false;if(b.here.from_coverflow)b.coverflow=true;b.here=b.parents[--b.depth];navigate();
    if(b.here.view==ALBUM_ARTISTS&&rebuild_owners)catalog_begin(true);
    if(b.here.view==ACTIVITY){b.here.count=b.activity_count;if(b.activity_dirty){b.activity_dirty=false;b.activity_count=b.activity_scan=b.activity_page=b.scan_id_count=b.scan_id_at=0;if(b.activity_kind!=11u)__builtin_memset(activity_data.filter,0,sizeof activity_data.filter);b.activity_scanning=true;b.activity_indexed=b.activity_waiting=false;
        b.activity_id_index_tried=false;
    }}
    if(b.here.view==LOCAL_TRACKS){uint32_t selected=b.here.selected,slot=b.here.slot,first=b.here.first;bool reverse=b.here.reverse;resolve_begin(slot);b.here.reverse=reverse;b.here.selected=selected;b.here.first=b.first=first;b.restored_selection=selected;}
    return true;
}
int ln_music_browser_queue_status(void){return b.status;}
bool ln_music_browser_take_queue(struct ln_music_browser_queue *out){if(b.status!=LN_BROWSER_QUEUE_READY||!out)return false;*out=b.queue;b.status=LN_BROWSER_QUEUE_IDLE;b.notice=0;return true;}
bool ln_music_browser_save_queue(const struct ln_queue *q){
    if(!q||!q->count||q->count>LINNET_QUEUE_CAPACITY){ln_music_browser_notice("Save limit: 1024 tracks");return false;}
    if(!ln_collections_writable()){ln_music_browser_notice(ln_collections_status());return false;}
    b.action_kind=0;b.export_queue=*q;b.exporting=true;b.export_at=0;b.draft=(struct ln_collection){0};b.choosing=true;b.notice=0;
    push();enter(DESTINATION,"Add to playlist");return true;
}
bool ln_music_browser_busy(void){return b.catalog||b.jumping||b.bookmark_waiting||b.favorite_waiting||b.activity_scanning||b.job!=NONE||b.range||b.activity_scanning||b.resolving||b.exporting||b.expanding||b.status==LN_BROWSER_QUEUE_LOADING;}
bool ln_music_browser_waiting(void){
    /* An accepted library job is serviced by the library/storage deadlines.
     * A hidden or suspended view retains navigation state without doing its
     * speculative list work. */
    if(b.job!=NONE)return true;
    if(b.bookmark_waiting||b.favorite_waiting){
        if(ln_music_history_error()||ln_music_history_record_error(b.favorite_id)||
           ln_music_history_record_ready(b.favorite_id))return false;
        return true;
    }
#if LINNET_TARGET_NUMBER==25u
    if(b.here.view==ACTIVITY&&b.activity_scanning&&b.activity_page<LN_HISTORY_PAGE_COUNT&&
       !ln_music_history_error()&&
       ((b.activity_kind==9&&ln_paged_history_favorite_page(b.activity_page))||
        (b.activity_kind==10&&ln_paged_history_bookmark_page(b.activity_page))||
        (b.activity_kind==12&&ln_paged_history_played_page(b.activity_page)))&&
       ln_paged_history_page_waiting(b.activity_page))return true;
#endif
    return b.suspended||(!b.visible_now&&!b.exporting&&!b.expanding&&
                         b.status!=LN_BROWSER_QUEUE_LOADING);
}
void ln_music_browser_step(void){
    if(ln_music_library_status()!=LN_LIBRARY_READY){b.job=NONE;b.was_ready=false;b.expanding=b.exporting=b.resolving=false;b.status=LN_BROWSER_QUEUE_IDLE;return;}
    if(!b.was_ready){b.was_ready=true;invalidate();}
    if(b.job!=NONE){
        enum job job=b.job;struct ln_music_group g;char name[256];struct ln_music_library_search_row title;struct ln_music_track track;uint32_t member=UINT32_MAX,id_count=0;union {uint64_t ids[LN_MUSIC_ID_BATCH];uint32_t members[LN_MUSIC_ID_BATCH];} batch;
        bool done=job==RECENT_IDS||job==ACTIVITY_IDS||job==RESOLVE_IDS?ln_music_library_take_track_ids(batch.ids,&id_count):(job==TITLE||job==JUMP_TITLE)?ln_music_library_take_view_row(&title):(job==CATALOG_MEMBER||job==CATALOG_ARTIST)?ln_music_library_take_members(batch.members,&id_count):(job==MEMBER||job==EXPAND)?ln_music_library_take_member(&member):job==RECORD||job==FAVORITE_RECORD||job==BOOKMARK_RECORD?ln_music_library_take_track_record(&track):ln_music_library_take_group(&g,name);
        if(!done)return;b.job=NONE;if(b.job_epoch!=b.epoch)return;
        if(job==ACTIVITY_IDS||job==RESOLVE_IDS){b.scan_id_count=id_count;b.scan_id_at=0;
            for(uint32_t i=0;i<id_count;i++)b.scan_ids[i]=batch.ids[i];
        }
        else if(job==RECENT_IDS){
            for(uint32_t i=0;i<id_count;i++){
                uint32_t score=ln_collections_recent_rank(batch.ids[i]);if(!score)continue;
                activity_insert(b.activity_scan+i,score);
            }
            b.activity_scan+=id_count;changed();
        }
        else if(job==JUMP_GROUP||job==JUMP_TITLE){
            jump_label(job==JUMP_GROUP?name:title.title);
        }
        else if(job==CATALOG_RANGE){if(g.kind<2)b.lo=b.mid+1;else b.hi=b.mid;}
        else if(job==CATALOG_ARTIST){
            for(uint32_t i=0;i<id_count;i++){member=batch.members[i];if(member<LINNET_QUEUE_SOURCE_MAX)activity_data.filter[member/8]|=(uint8_t)(1u<<(member&7));}
            b.catalog_member+=id_count;if(!id_count)b.catalog_member=b.here.slot;
        }
        else if(job==CATALOG_MEMBER){
            b.catalog_member+=id_count;
            for(uint32_t i=0;i<id_count;i++){member=batch.members[i];if(member<LINNET_QUEUE_SOURCE_MAX&&(activity_data.filter[member/8]&(1u<<(member&7)))){
                if(b.catalog_count<LINNET_QUEUE_SOURCE_MAX)activity_data.tracks[b.catalog_count++]=b.catalog_group;b.catalog=2;break;
            }}
            if(!id_count||b.catalog_member>=b.catalog_members)b.catalog=2;
        }
        else if(job==CATALOG_GROUP){b.catalog_at++;
            if(g.kind>2)b.catalog_at=b.catalog_end;
            if(g.kind==2){if(b.here.view==ALBUM_ARTISTS){if(g.parent<sizeof activity_data.filter*8)activity_data.filter[g.parent/8]|=(uint8_t)(1u<<(g.parent&7));}
                else if(g.parent==b.here.group){if(b.catalog_count<LINNET_QUEUE_SOURCE_MAX)activity_data.tracks[b.catalog_count++]=b.catalog_at-1;}
                else if(g.count){b.catalog_group=b.catalog_at-1;b.catalog_members=g.count;b.catalog_member=0;b.catalog=3;}}
        }
        else if(job==RANGE){uint32_t target=b.here.kind+(b.range==2);if(g.kind<target)b.lo=b.mid+1;else b.hi=b.mid;}
        else if(job==EXPAND){if(member>=ln_music_library_count()){b.expanding=false;ln_music_browser_notice("Track unavailable");return;}b.expanded.tracks[b.expand_at]=member;b.expanded.order[b.expand_at]=b.expand_at;b.expand_at++;if(b.expand_at==b.expanded.count){b.expanding=false;b.expanded.source=0;b.expanded_ready=true;changed();}}
        else if(job==MEMBER){if(member>=ln_music_library_count()){b.status=LN_BROWSER_QUEUE_INVALID;b.exporting=false;ln_music_browser_notice("Track unavailable");return;}
            if(b.exporting){b.action_index=member;b.action_kind=1;}else{b.queue.tracks[b.next++]=member;if(b.next==b.queue.count){b.queue.source=0;b.status=LN_BROWSER_QUEUE_READY;}}}
        else if(job==RECENT_GROUP){b.recent_open=false;b.here.view=TRACKS;b.here.group=b.here.lower;b.here.count=g.count;copy(b.here.title,"Recently Added");navigate();}
        else if(job==BOOKMARK_RECORD){b.favorite_id=track.id;b.bookmark_waiting=true;}
        else if(job==FAVORITE_RECORD){b.favorite_id=track.id;b.favorite_waiting=true;}
        else if(job==RECORD){b.draft.ids[b.draft.count++]=track.id;b.export_at++;b.action_kind=0;if(b.export_at==b.export_queue.count){b.exporting=false;changed();}}
        else {struct row *r=&b.rows[b.job_row%(LN_MUSIC_BROWSER_ROWS*2)];*r=(struct row){.ordinal=b.job_row};
            if(job==TITLE){r->index=title.index;copy(r->label,title.title);}else{r->index=b.here.view==ARTIST_ALBUMS?activity_data.tracks[ordinal(b.job_row)-1]:b.here.view==ALBUM_ARTISTS?activity_data.tracks[ordinal(b.job_row)]:b.here.lower+ordinal(b.job_row);r->count=g.count;copy(r->label,name);}changed();}
    }
    if(b.bookmark_waiting){
        if(ln_music_history_error()||ln_music_history_record_error(b.favorite_id)){b.bookmark_waiting=false;ln_music_browser_notice("History unavailable");}
        else if(ln_music_history_record_ready(b.favorite_id)){const struct ln_music_journal_record *r=ln_music_history_get(b.favorite_id);b.bookmark_waiting=false;
            if(b.bookmark_remove){bool saved=ln_music_history_set_bookmark(b.favorite_id,false,0);b.bookmark_remove=false;b.activity_dirty=saved;ln_music_browser_notice(saved?"Bookmark removed":"Bookmark could not be removed");}
            else if(r&&(r->flags&LN_MUSIC_JOURNAL_BOOKMARK)){uint64_t frame=r->bookmark_frame;queue_begin(1,0,0,0,0);b.queue.tracks[0]=b.action_index;b.queue.frame=frame;b.status=LN_BROWSER_QUEUE_READY;}else ln_music_browser_notice("Bookmark no longer exists");
        }else (void)ln_music_history_prefetch(b.favorite_id);
    }
    if(b.favorite_waiting){
        if(ln_music_history_error()||ln_music_history_record_error(b.favorite_id)){b.favorite_waiting=false;ln_music_browser_notice("History unavailable");}
        else if(ln_music_history_record_ready(b.favorite_id)){const struct ln_music_journal_record *r=ln_music_history_get(b.favorite_id);bool favorite=!(r&&(r->flags&LN_MUSIC_JOURNAL_FAVORITE));
            if(ln_music_history_set_favorite(b.favorite_id,favorite)){b.activity_dirty=true;ln_music_browser_notice(favorite?"Added to Favorites":"Removed from Favorites");}else ln_music_browser_notice("Favorite could not be saved");b.favorite_waiting=false;
        }else (void)ln_music_history_prefetch(b.favorite_id);
    }
    if(b.suspended)return;
    if(!b.visible_now&&!b.exporting&&!b.expanding&&b.status!=LN_BROWSER_QUEUE_LOADING)return;
    if(b.catalog){
        if(b.catalog==1){if(b.catalog_member>=b.here.slot){b.catalog=4;b.catalog_member=0;}else if(ln_music_library_group_members(b.here.group,b.catalog_member))b.job=CATALOG_ARTIST;}
        if(b.catalog==4){if(b.lo==b.hi){b.catalog_at=b.lo;b.catalog=2;}else{b.mid=b.lo+(b.hi-b.lo)/2;if(ln_music_library_group_record(b.mid))b.job=CATALOG_RANGE;}}
        if(b.catalog==2){if(b.catalog_at==b.catalog_end){
                if(b.here.view==ALBUM_ARTISTS){for(uint32_t i=0;i<ln_music_library_group_count()&&i<sizeof activity_data.filter*8;i++)if(activity_data.filter[i/8]&(1u<<(i&7)))if(b.catalog_count<LINNET_QUEUE_SOURCE_MAX)activity_data.tracks[b.catalog_count++]=i;b.here.count=b.catalog_count;}
                else b.here.count=b.catalog_count+1;
                b.catalog=0;invalidate();return;
            }if(ln_music_library_group_record(b.catalog_at))b.job=CATALOG_GROUP;
        }else if(b.catalog==3&&ln_music_library_group_members(b.catalog_group,b.catalog_member))b.job=CATALOG_MEMBER;
    }else if(b.jumping&&!b.resolving){
        if(jump_cached())return;
        if(b.jump_binary){if(b.jump_lo==b.jump_hi){if(b.jump_found!=UINT32_MAX){b.jumping=false;b.here.selected=b.jump_found;navigate();}else jump_missing();return;}b.jump_at=b.jump_lo+(b.jump_hi-b.jump_lo)/2;}
        else if(b.jump_at>=b.here.count){jump_missing();return;}
        uint32_t at=ordinal(b.jump_at);
        if(b.here.view==ARTIST_ALBUMS&&!at){b.jump_at++;return;}
        if(b.here.view==GROUPS||b.here.view==ARTIST_ALBUMS||b.here.view==ALBUM_ARTISTS){uint32_t group=b.here.view==GROUPS?b.here.lower+at:activity_data.tracks[b.here.view==ARTIST_ALBUMS?at-1:at];if(ln_music_library_group(group))b.job=JUMP_GROUP;}
        else if(ln_music_library_view_row(b.here.view==TRACKS&&b.here.kind?2:0,b.here.group,b.here.view==ACTIVITY?activity_data.tracks[at]:b.here.view==LOCAL_TRACKS?b.mapped[at]:at))b.job=JUMP_TITLE;
    }else if(b.activity_scanning){
        if(activity_empty()){
            b.activity_scanning=false;b.here.count=b.activity_count=0;invalidate();return;
        }
        if(b.activity_kind==11){
            if(!ln_collections_ready())return;
            if(recent_cached())return;
            if(b.activity_indexed){b.activity_indexed=false;b.activity_scan=b.activity_count=0;}
            if(b.activity_scan>=ln_music_library_count()||b.activity_count>=ln_collections_recent_count()){
                b.activity_scanning=false;b.here.count=b.activity_count;invalidate();return;
            }
            if(ln_music_library_track_ids(b.activity_scan))b.job=RECENT_IDS;
            b.job_epoch=b.epoch;return;
        }
        if(b.activity_page<LN_HISTORY_PAGE_COUNT&&!ln_music_history_error()){
            /* A negative summary belongs to a validated page. Skip a bounded
             * run without dispatching again or requesting any page bytes. */
            uint32_t began=0,skipped=0;
            while((b.activity_kind==9&&!ln_paged_history_favorite_page(b.activity_page))||
               (b.activity_kind==10&&!ln_paged_history_bookmark_page(b.activity_page))||
               (b.activity_kind==12&&!ln_paged_history_played_page(b.activity_page))){
                if(!skipped)began=ln_time_us();
                b.activity_page++;
                if(b.activity_page==LN_HISTORY_PAGE_COUNT||++skipped==32u||
                   (uint32_t)(ln_time_us()-began)>=250u)return;
            }
            const struct ln_history_page *page=ln_paged_history_page(b.activity_page);if(!page)return;
            for(uint32_t i=0;i<page->count;i++){const struct ln_music_journal_record *r=&page->records[i];
                if((b.activity_kind==9&&(r->flags&LN_MUSIC_JOURNAL_FAVORITE))||(b.activity_kind==10&&(r->flags&LN_MUSIC_JOURNAL_BOOKMARK))||(b.activity_kind==12&&r->play_count))activity_filter(r->id,true);}
            b.activity_page++;if(!(b.activity_page&31))changed();return;
        }
        if(ln_music_history_error()){b.activity_scanning=false;ln_music_browser_notice("History unavailable");return;}
        if(b.activity_kind==12&&!b.activity_id_index_tried){
            uint64_t id;uint32_t ordinal;
            b.activity_id_index_tried=true;
            b.activity_indexed=ln_music_library_id_entry(0,&id,&ordinal);
        }
        if(b.activity_waiting){
            uint64_t id=b.activity_id;uint32_t score=0;
            {
                if(!ln_music_history_record_ready(id)){
                    if(ln_music_history_record_error(id)){
                        b.activity_waiting=false;b.activity_scan++;
                        if(!b.activity_indexed)b.scan_id_at++;
                        return;
                    }
                    (void)ln_music_history_prefetch(id);return;
                }
                const struct ln_music_journal_record *r=ln_music_history_get(id);
                if(r){score=b.activity_kind==9?((r->flags&LN_MUSIC_JOURNAL_FAVORITE)?1u:0u):b.activity_kind==10?((r->flags&LN_MUSIC_JOURNAL_BOOKMARK)?1u:0u):r->play_count;}
            }
            if(score&&(b.activity_kind==9||b.activity_kind==10)){
                activity_data.tracks[b.activity_count++]=b.activity_scan;
            }else if(score){uint32_t track=b.activity_scan;
                if(b.activity_indexed)track=b.activity_ordinal;
                activity_insert(track,score);
            }
            b.activity_scan++;
            if(!b.activity_indexed)b.scan_id_at++;
            b.activity_waiting=false;if(!(b.activity_scan&31))changed();
        }
        if(b.activity_scan>=ln_music_library_count()){b.activity_scanning=false;b.here.count=b.activity_count;if(b.activity_overflow)ln_music_browser_notice("Top 1024 most played");invalidate();return;}
        if(b.activity_kind==12&&b.activity_indexed){
            uint32_t started=ln_time_us();
            while(b.activity_scan<ln_music_library_count()){
                uint64_t id;uint32_t ordinal;
                if(!ln_music_library_id_entry(b.activity_scan,&id,&ordinal)){
                    b.activity_indexed=false;b.activity_scan=b.activity_count=0;break;
                }
                if(activity_filter(id,false)){
                    b.activity_id=id;b.activity_ordinal=ordinal;b.activity_waiting=true;return;
                }
                b.activity_scan++;
                if((uint32_t)(ln_time_us()-started)>=250u)break;
            }
            if(b.activity_indexed){
                if(b.activity_scan==ln_music_library_count()){b.activity_scanning=false;b.here.count=b.activity_count;
                    if(b.activity_overflow)ln_music_browser_notice("Top 1024 most played");invalidate();}
                else changed();
                return;
            }
        }
        if(b.scan_id_at<b.scan_id_count){
            b.activity_id=b.scan_ids[b.scan_id_at];
            if(!activity_filter(b.activity_id,false)){b.activity_scan++;b.scan_id_at++;if(!(b.activity_scan&31))changed();return;}
            b.activity_waiting=true;return;
        }
        if(ln_music_library_track_ids(b.activity_scan))b.job=ACTIVITY_IDS;
    }else if(b.expanding){uint32_t ordinal;if(ln_queue_track_at(&b.export_queue,b.expand_at,&ordinal)&&ln_music_library_group_member(b.export_queue.source_group,ordinal))b.job=EXPAND;
    }else if(b.exporting){uint32_t ordinal;if(!ln_queue_track_at(&b.export_queue,b.export_at,&ordinal)){b.exporting=false;ln_music_browser_notice("Queue changed");return;}
        if(b.export_queue.source==LN_QUEUE_GROUP&&!b.action_kind){if(ln_music_library_group_member(b.export_queue.source_group,ordinal))b.job=MEMBER;}
        else if(ln_music_library_track_record(b.export_queue.source==LN_QUEUE_GROUP?b.action_index:ordinal))b.job=RECORD;
    }else if(b.resolving){if(!ln_music_library_count()){b.resolving=false;b.here.count=0;}
        else if(b.indexed){uint32_t started=ln_time_us();
            do{uint32_t ordinal;
                if(!ln_music_library_track_id_lookup(b.draft.ids[b.scan],&ordinal)){resolve_prepare(false);break;}
                b.mapped[b.scan++]=ordinal;
            }while(b.scan<b.draft.count&&(uint32_t)(ln_time_us()-started)<250u);
            if(b.indexed&&b.scan==b.draft.count)resolve_complete();}
        else if(b.sort_at)resolve_sift(--b.sort_at,b.sort_end);
        else if(b.sort_end>1u){uint32_t last=--b.sort_end,position=b.positions[last];b.positions[last]=b.positions[0];b.positions[0]=position;resolve_sift(0,last);}
        else if(b.scan_id_at<b.scan_id_count){uint32_t started=ln_time_us();
            /* Consume only the ready batch within a bounded metadata slice. */
            do{resolve_id(b.scan_ids[b.scan_id_at++]);b.scan++;
                if(b.resolve_count==b.draft.count||b.scan==ln_music_library_count()){resolve_complete();break;}
            }while(b.scan_id_at<b.scan_id_count&&(uint32_t)(ln_time_us()-started)<250u);}
        else if(ln_music_library_track_ids(b.scan))b.job=RESOLVE_IDS;
    }else if(b.status==LN_BROWSER_QUEUE_LOADING){if(queue_cached())return;if(ln_music_library_group_member(b.queue.source_group,b.queue.source_group&LN_QUEUE_REVERSE?b.queue.count-1-b.next:b.next))b.job=MEMBER;
    }else if(b.recent_open){if(ln_music_library_group(b.here.lower))b.job=RECENT_GROUP;
    }else if(b.range){if(b.lo==b.hi){if(b.range==1){b.here.lower=b.lo;b.range=2;b.hi=ln_music_library_group_count();}else{b.here.count=b.lo-b.here.lower;b.range=0;if(b.here.kind==8&&b.here.count)b.recent_open=true;changed();}return;}
        b.mid=b.lo+(b.hi-b.lo)/2;if(ln_music_library_group_record(b.mid))b.job=RANGE;
    }else if(list_view()){
        for(uint32_t i=b.first;i<b.here.count&&i<b.first+b.visible*2;i++){if(row(i))continue;b.job_row=i;
            uint32_t at=ordinal(i);
            if(b.here.view==ARTIST_ALBUMS&&!at){struct row *r=&b.rows[i%(LN_MUSIC_BROWSER_ROWS*2)];*r=(struct row){.ordinal=i,.index=b.here.group,.count=b.here.slot};copy(r->label,"All Songs");changed();continue;}
            if(b.here.view==GROUPS||b.here.view==ARTIST_ALBUMS||b.here.view==ALBUM_ARTISTS){uint32_t group=b.here.view==GROUPS?b.here.lower+at:activity_data.tracks[b.here.view==ARTIST_ALBUMS?at-1:at];if(ln_music_library_group(group))b.job=GROUP;}
            else if(ln_music_library_view_row((b.here.view==LOCAL_TRACKS||b.here.view==ACTIVITY)?0:b.here.kind?2:1,b.here.group,b.here.view==LOCAL_TRACKS?b.mapped[at]:b.here.view==ACTIVITY?activity_data.tracks[at]:at))b.job=TITLE;break;}
    }
    if(b.job!=NONE)b.job_epoch=b.epoch;
}

bool ln_music_browser_expand(const struct ln_queue *q){
    if(!q||q->count>LINNET_QUEUE_CAPACITY||b.expanding||b.exporting)return false;
    b.export_queue=*q;b.expanded=*q;b.expand_at=0;b.expanded_ready=false;
    if(q->source==LN_QUEUE_GROUP){b.expanding=true;return true;}
    for(uint32_t i=0;i<q->count;i++){uint32_t index;if(!ln_queue_track_at(q,i,&index))return false;b.expanded.tracks[i]=index;b.expanded.order[i]=i;}
    b.expanded.source=0;b.expanded_ready=true;return true;
}
bool ln_music_browser_take_expanded(struct ln_queue *out){if(!out||!b.expanded_ready)return false;*out=b.expanded;b.expanded_ready=false;return true;}

bool ln_music_browser_external_actions(uint32_t index,uint32_t kind,uint32_t count,const char *title){
    if(b.exporting||b.expanding||!push())return false;
    b.action_track=!kind;b.action_bookmark=false;b.action_local=b.empty_actions=false;b.action_index=index;b.action_group=index&~LN_MUSIC_SEARCH_GROUP;b.action_count=count;b.action_kind=kind;b.external=true;
    enter(ACTIONS,title);return true;
}
bool ln_music_browser_take_search_return(void){bool value=b.search_return;b.search_return=false;return value;}

const char *ln_music_browser_hint(void){
    if(list_view())return "Left: Browse  Right: Actions";
    if(b.here.view==PLAYLISTS)return "Right: Actions  Menu: Back";
    return "Select: Choose  Menu: Back";
}
