#ifndef INCLUDE_NARF_MBR_H
#define INCLUDE_NARF_MBR_H

///////////////////////////////////////////////////////
//! @brief Classic MBR Partition Entry (16 bytes)
typedef struct PACKED {
   uint8_t  boot_indicator;
   uint8_t  start_head;
   uint8_t  start_sector;
   uint8_t  start_cylinder;
   uint8_t  partition_type;
   uint8_t  end_head;
   uint8_t  end_sector;
   uint8_t  end_cylinder;
   uint32_t start_lba;
   uint32_t partition_size;
} MBRPartitionEntry;
static_assert(sizeof(MBRPartitionEntry) == 16, "MBRPartitionEntry wrong size");

// See https://aeb.win.tue.nl/partitions/partition_types-1.html
// there is no assigned numbers authority for MBR partition types
// this one seemed to have the least claim, so we're squatting.
#define NARF_PART_TYPE 0x6E

typedef struct PACKED {
   uint8_t boot_code[446];
   MBRPartitionEntry partitions[4];
   uint16_t signature;
} MBR;
static_assert(sizeof(MBR) == 512, "MBR wrong size");
#define MBR_SIGNATURE 0xAA55

#endif
