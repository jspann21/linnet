#include "queue_view.h"
#include "library.h"
#include "hal/api/platform.h"
static struct {
    /* Only this private immutable virtual snapshot uses tracks[0..31] for
     * sorted exclusion ranks and order[0] as their validity flag. Every whole
     * snapshot assignment below clears that flag; explicit queues keep both
     * arrays as queue content. Never mutate a virtual permutation in place. */
    struct ln_queue queue;
    char labels[LN_QUEUE_VIEW_ROWS][259];
    /* Zero is invalid; other values bound the complete label in byte pairs. */
    uint8_t valid[LN_QUEUE_VIEW_ROWS];
    bool opened,pending,was_ready;
    uint32_t queue_revision,first,epoch,revision,job_epoch,job_row,visible_rows;
} view;
static void invalidate(void){
    ++view.epoch;++view.revision;
    for(uint32_t i=0;i<LN_QUEUE_VIEW_ROWS;i++)view.valid[i]=false;
}
static bool same_labels(const struct ln_queue *a,const struct ln_queue *b){
    if(a->source!=b->source||a->source_group!=b->source_group||a->count!=b->count)return false;
    for(uint32_t i=0;i<32u;i++)if(a->library_identity[i]!=b->library_identity[i])return false;
    if(a->source){
        if(a->stride!=b->stride||a->offset!=b->offset||a->inverse!=b->inverse||
           a->source_count!=b->source_count||a->excluded_count!=b->excluded_count)return false;
        for(uint32_t i=0;i<a->excluded_count;i++)if(a->excluded[i]!=b->excluded[i])return false;
    }else for(uint32_t i=0;i<a->count;i++)
        if(a->tracks[i]!=b->tracks[i]||a->order[i]!=b->order[i])return false;
    return true;
}
void ln_music_queue_view_update(const struct ln_queue *queue,uint32_t queue_revision){
    if(!queue||!view.opened||view.queue_revision==queue_revision)return;
    bool retained=same_labels(&view.queue,queue);
    view.queue=*queue;view.queue_revision=queue_revision;
    if(view.queue.source)view.queue.order[0]=0;
    if(view.first>=queue->count)view.first=queue->count?queue->count-1u:0;
    if(!retained){invalidate();return;}
    bool changed=false;
    for(uint32_t i=0;i<LN_QUEUE_VIEW_ROWS&&view.first+i<queue->count;i++)if(view.valid[i]){
        char marker=view.first+i==queue->cursor?'>':' ';
        if(view.labels[i][0]!=marker){view.labels[i][0]=marker;changed=true;}
    }
    if(changed)++view.revision;
}
void ln_music_queue_view_open(const struct ln_queue *queue,uint32_t queue_revision){
    if(!queue)return;
    view.opened=true;view.queue=*queue;view.queue_revision=queue_revision;
    if(view.queue.source)view.queue.order[0]=0;
    if(!view.visible_rows)view.visible_rows=6u;
    view.was_ready=ln_music_library_status()==LN_LIBRARY_READY;
    view.first=queue->count?(queue->cursor/6u)*6u:0;invalidate();
}
void ln_music_queue_view_close(void){view.opened=false;invalidate();}
void ln_music_queue_view_visible_rows(uint32_t rows){
    view.visible_rows=rows?rows:1u;
    if(view.visible_rows>LN_QUEUE_VIEW_ROWS)view.visible_rows=LN_QUEUE_VIEW_ROWS;
}
void ln_music_queue_view_window(uint32_t first){
    if(!view.opened)return;
    if(first>=view.queue.count)first=view.queue.count?view.queue.count-1u:0;
    if(view.first!=first){
        uint32_t old=view.first;
        for(uint32_t n=0;n<LN_QUEUE_VIEW_ROWS;n++){
            uint32_t i=first>old?n:LN_QUEUE_VIEW_ROWS-1u-n,row=first+i;
            if(row>=old&&row-old<LN_QUEUE_VIEW_ROWS){
                uint32_t source=row-old;
                uint32_t units=view.valid[source];
                if(units)__builtin_memcpy(view.labels[i],view.labels[source],units*2u);
                view.valid[i]=(uint8_t)units;
            }else view.valid[i]=false;
        }
        view.first=first;++view.revision;
    }
}
uint32_t ln_music_queue_view_count(void){return view.opened?view.queue.count:0;}
uint32_t ln_music_queue_view_revision(void){return view.revision;}
bool ln_music_queue_view_busy(void){
    if(view.pending)return true;
    if(view.opened)for(uint32_t i=0;i<LN_QUEUE_VIEW_ROWS&&view.first+i<view.queue.count;i++)
        if(!view.valid[i])return true;
    return false;
}
const char *ln_music_queue_view_label(uint32_t row){
    if(!view.opened||row>=view.queue.count)return "";
    if(row<view.first||row-view.first>=LN_QUEUE_VIEW_ROWS||!view.valid[row-view.first])return "Loading...";
    return view.labels[row-view.first];
}
static void publish(uint32_t row,const char *title,bool notify){
    if(row<view.first||row-view.first>=LN_QUEUE_VIEW_ROWS)return;
    uint32_t slot=row-view.first;char *out=view.labels[slot];
    out[0]=row==view.queue.cursor?'>':' ';out[1]=' ';
    uint32_t i=0;while(i<255u&&title[i]){out[i+2]=title[i];i++;}out[i+2]=0;
    view.valid[slot]=(uint8_t)((i+4u)>>1);
    /* Publish one complete viewport; speculative lookahead never repaints. */
    if(!notify||slot>=view.visible_rows)return;
    for(uint32_t n=0;n<view.visible_rows&&view.first+n<view.queue.count;n++)
        if(!view.valid[n])return;
    ++view.revision;
}
static const char *cached_title(uint32_t ordinal){
    if(view.queue.source>LN_QUEUE_GROUP)return 0;
    if(view.queue.source==LN_QUEUE_GROUP&&
       !ln_music_library_cached_member(view.queue.source_group&~LN_QUEUE_REVERSE,ordinal,&ordinal))return 0;
    return ln_music_library_title(ordinal);
}
static bool excluded_track_at(uint32_t at,uint32_t *track){
    if(!view.queue.order[0]){
        if(!ln_queue_exclusion_ranks(&view.queue,view.queue.tracks))return false;
        view.queue.order[0]=1u;
    }
    return ln_queue_track_at_ranks(&view.queue,at,view.queue.tracks,track);
}
bool ln_music_queue_view_prepare(void){
    if(!view.opened||view.queue.source>LN_QUEUE_GROUP||ln_music_library_status()!=LN_LIBRARY_READY)return false;
    union {uint8_t identity[32];struct ln_music_group group;} local;
    if(!ln_music_library_identity(local.identity))return false;
    for(uint32_t i=0;i<32u;++i)if(local.identity[i]!=view.queue.library_identity[i])return false;
    bool filled=false;uint32_t started=ln_time_us();
    local.group.count=0;
    for(uint32_t i=0;i<view.visible_rows&&view.first+i<view.queue.count&&
        (uint32_t)(ln_time_us()-started)<250u;++i){
        if(view.valid[i])continue;uint32_t track;
        if(!(view.queue.source&&view.queue.excluded_count?
             excluded_track_at(view.first+i,&track):ln_queue_track_at(&view.queue,view.first+i,&track)))continue;
        if(view.queue.source==LN_QUEUE_GROUP){
            if(!local.group.count&&
               !ln_music_library_cached_group(view.queue.source_group&~LN_QUEUE_REVERSE,&local.group,0))continue;
            if(track>=local.group.count||
               !ln_music_library_cached_member_at(local.group.first+track,&track))continue;
        }
        const char *title=ln_music_library_title(track);
        if(title){publish(view.first+i,title,false);filled=true;}
    }
    return filled;
}
void ln_music_queue_view_step(bool allow_query){
    if(ln_music_library_status()!=LN_LIBRARY_READY){
        view.pending=false;view.was_ready=false;
        if(view.opened)for(uint32_t i=0;i<LN_QUEUE_VIEW_ROWS&&view.first+i<view.queue.count;i++)
            if(!view.valid[i])publish(view.first+i,"Library unavailable",true);
        return;
    }
    if(!view.was_ready){view.was_ready=true;if(view.opened)invalidate();}
    if(view.pending){
        struct ln_music_library_search_row row;
        if(!ln_music_library_take_view_row(&row))return;
        view.pending=false;
        if(view.opened&&view.job_epoch==view.epoch)
            publish(view.job_row,row.index==UINT32_MAX?"Unavailable track":row.title,true);
    }
    if(!view.opened||!allow_query)return;
    for(uint32_t i=0;i<LN_QUEUE_VIEW_ROWS&&view.first+i<view.queue.count;i++){
        if(view.valid[i])continue;uint32_t ordinal;
        /* Completed rows need no query admission. Validate the library before
         * resolving the first missing row, after draining pending/reload work. */
        uint8_t identity[32];bool matched=ln_music_library_identity(identity);
        for(uint32_t n=0;n<32&&matched;n++)if(identity[n]!=view.queue.library_identity[n])matched=false;
        if(!matched){publish(view.first+i,"Library changed",true);return;}
        if(!(view.queue.source&&view.queue.excluded_count?
             excluded_track_at(view.first+i,&ordinal):ln_queue_track_at(&view.queue,view.first+i,&ordinal))){publish(view.first+i,"Invalid queue entry",true);return;}
        const char *title=cached_title(ordinal);
        if(title){publish(view.first+i,title,true);return;}
        if(ln_music_library_view_row(view.queue.source,view.queue.source_group,ordinal)){
            view.pending=true;view.job_epoch=view.epoch;view.job_row=view.first+i;
        }
        return;
    }
}
bool ln_music_queue_view_query_pending(void){return view.pending;}
