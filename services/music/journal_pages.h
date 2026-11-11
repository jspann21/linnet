#ifndef LINNET_MUSIC_JOURNAL_PAGES_H
#define LINNET_MUSIC_JOURNAL_PAGES_H
#include "history_record.h"
#define LN_HISTORY_PAGE_BYTES 4096u
#define LN_HISTORY_PAGE_COUNT 1024u
#define LN_HISTORY_PAGE_RECORDS 125u
#define LN_HISTORY_PAGED_BANK_BYTES ((LN_HISTORY_PAGE_COUNT+1u)*LN_HISTORY_PAGE_BYTES)
struct ln_history_page {
    struct ln_music_journal_record records[LN_HISTORY_PAGE_RECORDS];
    uint8_t epoch[16];
    uint32_t index,count,sequence,revision,durable_revision,bank;
    uint32_t staged_revision,staged_sequence,staged_bank;
    bool ready,staged;
};
/* HIST superblock and HPGE data pages are4096 bytes, CRC32 at4092. Both
 * carry a zero reserved field and header size64. Superblock: totalbytes@8,pagecount@12,slots@16,
 * recordbytes@20,epoch16@24; zero40..4091. Page:index@8,sequence@12,
 * count@16,event revision@20,occupied bitmap16@24,epoch16@40,zero56..63.
 * Records@64 use the32-byte history encoding; occupied slots form a dense prefix;
 * unused records/padding are zero. No deletion or silent eviction. */
bool ln_history_superblock(const uint8_t *,uint8_t epoch[16]);
uint32_t ln_history_page_hash(uint64_t id);
int ln_history_page_load(struct ln_history_page *,const uint8_t *,const uint8_t *,uint32_t index,const uint8_t epoch[16]);
const struct ln_music_journal_record *ln_history_page_get(const struct ln_history_page *,uint64_t);
bool ln_history_page_put(struct ln_history_page *,const struct ln_music_journal_record *);
bool ln_history_page_encode(struct ln_history_page *,uint8_t out[LN_HISTORY_PAGE_BYTES]);
bool ln_history_page_ack(struct ln_history_page *);
#endif
