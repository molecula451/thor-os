//=======================================================================
// Copyright Baptiste Wicht 2013.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#include "fat32.hpp"
#include "console.hpp"

#include "stl/types.hpp"
#include "stl/unique_ptr.hpp"
#include "stl/algorithms.hpp"
#include "stl/pair.hpp"

namespace {

//FAT 32 Boot Sector
struct fat_bs_t {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t number_of_fat;
    uint16_t root_directories_entries;
    uint16_t total_sectors;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;
    uint32_t sectors_per_fat_long;
    uint16_t drive_description;
    uint16_t version;
    uint32_t root_directory_cluster_start;
    uint16_t fs_information_sector;
    uint16_t boot_sectors_copy_sector;
    uint8_t filler[12];
    uint8_t physical_drive_number;
    uint8_t reserved;
    uint8_t extended_boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char file_system_type[8];
    uint8_t boot_code[420];
    uint16_t signature;
}__attribute__ ((packed));

//FAT 32 Information sector
struct fat_is_t {
    uint32_t signature_start;
    uint8_t reserved[480];
    uint32_t signature_middle;
    uint32_t free_clusters;
    uint32_t allocated_clusters;
    uint8_t reserved_2[12];
    uint32_t signature_end;
}__attribute__ ((packed));

static_assert(sizeof(fat_bs_t) == 512, "FAT Boot Sector is exactly one disk sector");

//An entry in the directory cluster
struct cluster_entry {
    char name[11];
    uint8_t attrib;
    uint8_t reserved;
    uint8_t creation_time_seconds;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t accessed_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__ ((packed));

//A long file name text entry in the directory cluster
struct long_entry {
    uint8_t sequence_number;
    uint16_t name_first[5];
    uint8_t attrib;
    uint8_t reserved;
    uint8_t alias_checksum;
    uint16_t name_second[6];
    uint16_t starting_cluster;
    uint16_t name_third[2];
} __attribute__ ((packed));

static_assert(sizeof(cluster_entry) == 32, "A cluster entry is 32 bytes");
static_assert(sizeof(long_entry) == 32, "A cluster entry is 32 bytes");

uint64_t cached_disk = -1;
uint64_t cached_partition = -1;
uint64_t partition_start;

fat_bs_t* fat_bs = nullptr;
fat_is_t* fat_is = nullptr;

void cache_bs(fat32::dd disk, const disks::partition_descriptor& partition){
    std::unique_ptr<fat_bs_t> fat_bs_tmp(new fat_bs_t());

    if(read_sectors(disk, partition.start, 1, fat_bs_tmp.get())){
        fat_bs = fat_bs_tmp.release();

        //TODO fat_bs->signature should be 0xAA55
        //TODO fat_bs->file_system_type should be FAT32
    } else {
        fat_bs = nullptr;
    }
}

void cache_is(fat32::dd disk, const disks::partition_descriptor& partition){
    auto fs_information_sector = partition.start + static_cast<uint64_t>(fat_bs->fs_information_sector);

    std::unique_ptr<fat_is_t> fat_is_tmp(new fat_is_t());

    if(read_sectors(disk, fs_information_sector, 1, fat_is_tmp.get())){
        fat_is = fat_is_tmp.release();

        //TODO fat_is->signature_start should be 0x52 0x52 0x61 0x41
        //TODO fat_is->signature_middle should be 0x72 0x72 0x41 0x61
        //TODO fat_is->signature_end should be 0x00 0x00 0x55 0xAA
    } else {
        fat_is = nullptr;
    }
}

//Cache information about the disk and the partition
bool cache_disk_partition(fat32::dd disk, const disks::partition_descriptor& partition){
    if(cached_disk != disk.uuid || cached_partition != partition.uuid){
        partition_start = partition.start;

        cache_bs(disk, partition);
        cache_is(disk, partition);

        cached_disk = disk.uuid;
        cached_partition = partition.uuid;
    }

    //Something may go wrong when reading the two base vectors
    return fat_bs && fat_is;
}

//Write information sector to the disk
bool write_is(fat32::dd disk, const disks::partition_descriptor& partition){
    auto fs_information_sector = partition.start + static_cast<uint64_t>(fat_bs->fs_information_sector);

    return write_sectors(disk, fs_information_sector, 1, fat_is);
}

//Return the absolute sector where the cluster resides
uint64_t cluster_lba(uint64_t cluster){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;
    uint64_t cluster_begin = fat_begin + (fat_bs->number_of_fat * fat_bs->sectors_per_fat_long);

    return cluster_begin + (cluster - 2 ) * fat_bs->sectors_per_cluster;
}

//Return the value of the fat for the given cluster
//Return 0 if an error occurs
uint32_t read_fat_value(fat32::dd disk, uint32_t cluster){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;
    uint64_t fat_sector = fat_begin + (cluster * sizeof(uint32_t)) / 512;

    std::unique_heap_array<uint32_t> fat_table(512 / sizeof(uint32_t));
    if(read_sectors(disk, fat_sector, 1, fat_table.get())){
        uint64_t entry_offset = cluster % 512;
        return fat_table[entry_offset] & 0x0FFFFFFF;
    } else {
        return 0;
    }
}

//Write a value to the FAT for the given cluster
bool write_fat_value(fat32::dd disk, uint32_t cluster, uint32_t value){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;
    uint64_t fat_sector = fat_begin + (cluster * sizeof(uint32_t)) / 512;

    //Read the cluster we need to alter
    std::unique_heap_array<uint32_t> fat_table(512 / sizeof(uint32_t));
    if(!read_sectors(disk, fat_sector, 1, fat_table.get())){
        return false;
    }

    //Set the entry to the given value
    uint64_t entry_offset = cluster % 512;
    fat_table[entry_offset] = value;

    //Write back the cluster
    return write_sectors(disk, fat_sector, 1, fat_table.get());
}

//Return the next cluster in the chain for the give cluster
//0 indicates that there is no next cluster
uint32_t next_cluster(fat32::dd disk, uint32_t cluster){
    auto fat_value = read_fat_value(disk, cluster);
    if(fat_value >= 0x0FFFFFF8){
        return 0;
    }

    return fat_value;
}

//Return the size of the fat in sectors
uint32_t fat_size(){
    return fat_bs->sectors_per_fat_long + fat_bs->sectors_per_fat;
}

//Find a free cluster in the disk
//0 indicates failure or disk full
uint32_t find_free_cluster(fat32::dd disk){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;

    auto entries_per_sector = 512 / sizeof(uint32_t);

    std::unique_heap_array<uint32_t> fat_table(entries_per_sector);

    for(size_t j = 0; j < fat_size(); ++j){
        uint64_t fat_sector = fat_begin + j * fat_bs->sectors_per_cluster;

        if(!read_sectors(disk, fat_sector, 1, fat_table.get())){
            return 0; //0 is not a valid cluster number, indicates failure
        }

        for(size_t i = 0; i < fat_table.size(); ++i){
            //Cluster 0 and 1 are not valid cluster
            if(j == 0 && i < 2){
                continue;
            }

            auto value = fat_table[i] & 0x0FFFFFFF;

            if(value == 0x0){
                return i + j * entries_per_sector;
            }
        }
    }

    return 0; //0 is not a valid cluster number, indicates failure
}

//Indicates if the entry is unused, indicating a file deletion or move
inline bool entry_unused(const cluster_entry& entry){
    return entry.name[0] == 0xE5;
}

//Indicates if the entry is used, or namely, not unused
inline bool entry_used(const cluster_entry& entry){
    return entry.name[0] != 0xE5;
}

//Indicates if the entry marks the end of the directory
inline bool end_of_directory(const cluster_entry& entry){
    return entry.name[0] == 0x0;
}

//Indicates if the entry denotes a long file name entry
inline bool is_long_name(const cluster_entry& entry){
    return entry.attrib == 0x0F;
}

//Return all the files of the given directory (denoted by its cluster number)
std::vector<disks::file> files(fat32::dd disk, uint32_t cluster_number){
    std::vector<disks::file> files;

    bool end_reached = false;

    bool long_name = false;
    char long_name_buffer[256];
    size_t i = 0;

    std::unique_heap_array<cluster_entry> cluster(16 * fat_bs->sectors_per_cluster);

    while(!end_reached){
        if(!read_sectors(disk, cluster_lba(cluster_number), fat_bs->sectors_per_cluster, cluster.get())){
            return std::move(files);
        }

        for(auto& entry : cluster){
            if(end_of_directory(entry)){
                end_reached = true;
                break;
            }

            if(entry_used(entry)){
                if(is_long_name(entry)){
                    auto& l_entry = reinterpret_cast<long_entry&>(entry);
                    long_name = true;

                    size_t l = ((l_entry.sequence_number & ~0x40) - 1) * 13;

                    bool end = false;
                    for(size_t j = 0; !end && j < 5; ++j){
                        if(l_entry.name_first[j] == 0x0 || l_entry.name_first[j] == 0xFF){
                            end = true;
                        } else {
                            char c = static_cast<char>(l_entry.name_first[j]);
                            long_name_buffer[l++] = c;
                        }
                    }

                    for(size_t j = 0; !end && j < 6; ++j){
                        if(l_entry.name_second[j] == 0x0 || l_entry.name_second[j] == 0xFF){
                            end = true;
                        } else {
                            char c = static_cast<char>(l_entry.name_second[j]);
                            long_name_buffer[l++] = c;
                        }
                    }

                    for(size_t j = 0; !end && j < 2; ++j){
                        if(l_entry.name_third[j] == 0x0 || l_entry.name_third[j] == 0xFF){
                            end = true;
                        } else {
                            char c = static_cast<char>(l_entry.name_third[j]);
                            long_name_buffer[l++] = c;
                        }
                    }

                    if(l > i){
                        i = l;
                    }

                    continue;
                }


                disks::file file;

                if(long_name){
                    for(size_t s = 0; s < i; ++s){
                        file.file_name += long_name_buffer[s];
                    }

                    long_name = false;
                    i = 0;
                } else {
                    //It is a normal file name
                    //Copy the name until the first space

                    for(size_t s = 0; s < 11; ++s){
                        if(entry.name[s] == ' '){
                            break;
                        }

                        file.file_name += entry.name[s];
                    }
                }

                file.hidden = entry.attrib & 0x1;
                file.system = entry.attrib & 0x2;
                file.directory = entry.attrib & 0x10;

                if(file.directory){
                    file.size = fat_bs->sectors_per_cluster * 512;
                } else {
                    file.size = entry.file_size;
                }

                file.location = entry.cluster_low + (entry.cluster_high << 16);

                files.push_back(file);
            }
        }

        if(!end_reached){
            cluster_number = next_cluster(disk, cluster_number);

            //If there are no more cluster, return false
            if(!cluster_number){
                return std::move(files);
            }

            //The block is corrupted
            if(cluster_number == 0x0FFFFFF7){
                return std::move(files);
            }
        }
    }

    return std::move(files);
}

//Find the cluster for the given path
std::pair<bool, uint32_t> find_cluster_number(fat32::dd disk, const std::vector<std::string>& path){
    auto cluster_number = fat_bs->root_directory_cluster_start;

    if(path.empty()){
        return std::make_pair(true, cluster_number);
    }

    std::unique_heap_array<cluster_entry> current_cluster(16 * fat_bs->sectors_per_cluster);

    for(size_t i = 0; i < path.size(); ++i){
        auto& p = path[i];

        bool found = false;

        auto entries = files(disk, cluster_number);

        for(auto& file : entries){
            if(i == path.size() - 1 || file.directory){
                if(file.file_name == p){
                    cluster_number = file.location;

                    //If it is the last part of the path, just return the
                    //number
                    if(i == path.size() - 1){
                        return std::make_pair(true, cluster_number);
                    }

                    //Otherwise, continue with the next level of the
                    //path

                    found = true;

                    break;
                }
            }
        }

        if(!found){
            return std::make_pair(false, 0);
        }
    }

    return std::make_pair(false, 0);
}

//Return all the files in the directory denoted by its path
//No test is make to verify that the path denotes a directory
std::vector<disks::file> files(fat32::dd disk, const std::vector<std::string>& path){
    auto cluster_number_search = find_cluster_number(disk, path);
    if(!cluster_number_search.first){
        return {};
    }

    return files(disk, cluster_number_search.second);
}

//Return the number of entries necessary to hold the name
//Always computed to store a long file name entry before the information entry
size_t number_of_entries(const std::string& name){
    return (name.size() - 1) / 11 + 2;
}

//Finds "entries" consecutive free entries in the given directory cluster
cluster_entry* find_free_entry(std::unique_heap_array<cluster_entry>& directory_cluster, size_t entries){
    size_t end;
    bool end_found = false;

    //1. Search the first end of directory marker
    for(size_t i = 0; i < directory_cluster.size(); ++i){
        auto& entry = directory_cluster[i];

        if(end_of_directory(entry)){
            end = i;
            end_found = true;
            break;
        }
    }

    if(!end_found){
        k_print_line("Unsupported free");
        return nullptr;
    }

    size_t sequence_size = 0;
    size_t sequence_start = 0;
    size_t sequence_end = 0;

    for(size_t i = 0; i < directory_cluster.size(); ++i){
        auto& entry = directory_cluster[i];

        if(end_of_directory(entry) || entry_unused(entry)){
            ++sequence_size;

            if(sequence_size == entries){
                sequence_start = i - (sequence_size - 1);
                sequence_end = i;
                break;
            }
        } else {
            sequence_size = 0;
        }
    }

    if(sequence_size != entries){
        //TODO Read the next cluster to find an empty entry
        k_print_line("Unsupported free");
        return nullptr;
    }

    //If the free sequence position is after end or if end  is inside the free
    //sequence
    if(end <= sequence_end){
        int64_t new_end = -1;
        for(size_t i = directory_cluster.size() - 1; i > 0; --i){
            if(i <= sequence_end){
                break;
            }

            auto& entry = directory_cluster[i];
            if(end_of_directory(entry) || entry_unused(entry)){
                new_end = i;
            } else {
                break;
            }
        }

        if(new_end < 0){
            //TODO Read the next cluster to find an empty entry
            k_print_line("Unsupported free");
            return nullptr;
        }

        //Mark the old end as unused
        directory_cluster[end].name[0] = 0xE5;

        //Mark the new end as the end of the directory
        directory_cluster[new_end].name[0] = 0x0;
    }

    return &directory_cluster[sequence_start];
}

//Init an entry
template<bool Long>
cluster_entry* init_entry(cluster_entry* entry_ptr, const char* name, uint32_t cluster){
    //If necessary create all the long filename entries
    if(Long){
        auto len = std::str_len(name);
        size_t sequences = (len - 1) / 11 + 1;

        size_t sequence = 0;
        size_t i = 0;
        while(sequence < sequences){
            auto& l_entry = reinterpret_cast<long_entry&>(*entry_ptr);

            if(sequence == sequences - 1){
                l_entry.sequence_number = (sequence + 1) | 0x40;
            } else {
                l_entry.sequence_number = sequence + 1;
            }

            l_entry.attrib = 0x0F;
            l_entry.reserved = 0x0;
            l_entry.starting_cluster = 0x0;

            char sum = 0;

            for(int c = 0; c < 11; ++c){
                char v = c < len ? name[c] : ' ';
                sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + v;
            }

            l_entry.alias_checksum = sum;

            for(size_t j = 0; j < 5; ++j){
                if(i < len){
                    l_entry.name_first[j] = static_cast<uint16_t>(name[i++]);
                } else {
                    l_entry.name_first[j] = 0xFF;
                }
            }

            for(size_t j = 0; j < 6; ++j){
                if(i < len){
                    l_entry.name_second[j] = static_cast<uint16_t>(name[i++]);
                } else {
                    l_entry.name_second[j] = 0xFF;
                }
            }

            for(size_t j = 0; j < 2; ++j){
                if(i < len){
                    l_entry.name_third[j] = static_cast<uint16_t>(name[i++]);
                } else {
                    l_entry.name_third[j] = 0xFF;
                }
            }

            ++entry_ptr;
            ++sequence;
        }
    }

    auto& entry = *entry_ptr;

    //Copy the name into the entry
    size_t i = 0;
    for(; i < 11 && *name; ++i){
        entry.name[i] = *name++;
    }

    for(; i < 11; ++i){
        entry.name[i] = ' ';
    }

    //For now, date and time are not supported
    entry.creation_time = 0;
    entry.creation_time_seconds = 0;
    entry.creation_date = 0;
    entry.accessed_date = 0;
    entry.modification_date = 0;
    entry.modification_time = 0;

    //Clear reserved bits
    entry.reserved = 0;

    //By default file size is zero
    entry.file_size = 0;

    entry.cluster_low = static_cast<uint16_t>(cluster);
    entry.cluster_high = static_cast<uint16_t>(cluster >> 16);

    return entry_ptr;
}

//Init a directory entry
template<bool Long>
void init_directory_entry(cluster_entry* entry_ptr, const char* name, uint32_t cluster){
    //Init the base entry parameters
    entry_ptr = init_entry<Long>(entry_ptr, name, cluster);

    //Mark it as a directory
    entry_ptr->attrib = 1 << 4;
}

//Init a file entry
template<bool Long>
void init_file_entry(cluster_entry* entry_ptr, const char* name, uint32_t cluster){
    //Init the base entry parameters
    entry_ptr = init_entry<Long>(entry_ptr, name, cluster);

    //Mark it as a  file
    entry_ptr->attrib = 0;
}

} //end of anonymous namespace

uint64_t fat32::free_size(dd disk, const disks::partition_descriptor& partition){
    if(!cache_disk_partition(disk, partition)){
        return 0;
    }

    return fat_is->free_clusters * fat_bs->sectors_per_cluster * 512;
}

std::vector<disks::file> fat32::ls(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path){
    if(!cache_disk_partition(disk, partition)){
        return {};
    }

    return files(disk, path);
}

std::string fat32::read_file(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path, const std::string& file){
    if(!cache_disk_partition(disk, partition)){
        return {};
    }

    uint32_t cluster_number;

    size_t file_size = 0;
    auto found = false;
    auto all_files = files(disk, path);
    for(auto& f : all_files){
        if(f.file_name == file){
            found = true;
            file_size = f.size;
            cluster_number = f.location;
            break;
        }
    }

    //If the file is not found in the given directory, return empty content
    if(!found){
        return {};
    }

    //No need to read the cluster if there are no content
    if(file_size == 0){
        return {};
    }

    std::string content(file_size + 1);

    size_t read = 0;

    while(read < file_size){
        size_t cluster_size = 512 * fat_bs->sectors_per_cluster;
        std::unique_heap_array<char> cluster(cluster_size);

        if(read_sectors(disk, cluster_lba(cluster_number), fat_bs->sectors_per_cluster, cluster.get())){
            for(size_t i = 0; i < cluster_size && read < file_size; ++i,++read){
                content += cluster[i];
            }
        } else {
            break;
        }

        //If the file is not read completely, get the next cluster
        if(read < file_size){
            cluster_number = next_cluster(disk, cluster_number);

            //It may be possible that either the file size or the FAT entry is wrong
            if(!cluster_number){
                break;
            }

            //The block is corrupted
            if(cluster_number == 0x0FFFFFF7){
                break;
            }
        }
    }

    return std::move(content);
}

bool fat32::mkdir(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path, const std::string& directory){
    if(!cache_disk_partition(disk, partition)){
        return false;
    }

    //Find the cluster number of the parent directory
    auto cluster_number = find_cluster_number(disk, path);
    if(!cluster_number.first){
        return false;
    }

    //Find a free cluster to hold the directory entries
    auto cluster = find_free_cluster(disk);
    if(cluster == 0){
        return false;
    }

    std::unique_heap_array<cluster_entry> directory_cluster(16 * fat_bs->sectors_per_cluster);
    if(!read_sectors(disk, cluster_lba(cluster_number.second), fat_bs->sectors_per_cluster, directory_cluster.get())){
        return false;
    }

    auto entries = number_of_entries(directory);
    auto new_directory_entry = find_free_entry(directory_cluster, entries);

    init_directory_entry<true>(new_directory_entry, directory.c_str(), cluster);

    //Write back the parent directory cluster
    if(!write_sectors(disk, cluster_lba(cluster_number.second), fat_bs->sectors_per_cluster, directory_cluster.get())){
        return false;
    }

    //This cluster is the end of the chain
    if(!write_fat_value(disk, cluster, 0x0FFFFFF8)){
        return false;
    }

    //One cluster is now used for the directory entries
    fat_is->free_clusters -= 1;
    if(!write_is(disk, partition)){
        return false;
    }

    //Update the directory entries
    std::unique_heap_array<cluster_entry> new_directory_cluster(16 * fat_bs->sectors_per_cluster);

    auto dot_entry = &new_directory_cluster[0];
    init_directory_entry<false>(dot_entry, ".", cluster);

    auto dot_dot_entry = &new_directory_cluster[1];
    init_directory_entry<false>(dot_dot_entry, "..", cluster_number.second);

    //Mark everything as unused
    for(size_t j = 2; j < new_directory_cluster.size() - 1; ++j){
        new_directory_cluster[j].name[0] = 0xE5;
    }

    //End of directory
    new_directory_cluster[new_directory_cluster.size() - 1].name[0] = 0x0;

    //Write the directory entries to the disk
    if(!write_sectors(disk, cluster_lba(cluster), fat_bs->sectors_per_cluster, new_directory_cluster.get())){
        return false;
    }

    return true;
}

bool fat32::touch(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path, const std::string& file){
    if(!cache_disk_partition(disk, partition)){
        return false;
    }

    //Find the cluster number of the parent directory
    auto cluster_number = find_cluster_number(disk, path);
    if(!cluster_number.first){
        return false;
    }

    //Find a free cluster to hold the directory entries
    auto cluster = find_free_cluster(disk);
    if(cluster == 0){
        return false;
    }

    std::unique_heap_array<cluster_entry> directory_cluster(16 * fat_bs->sectors_per_cluster);
    if(!read_sectors(disk, cluster_lba(cluster_number.second), fat_bs->sectors_per_cluster, directory_cluster.get())){
        return false;
    }

    auto entries = number_of_entries(file);
    auto new_directory_entry = find_free_entry(directory_cluster, entries);

    init_file_entry<true>(new_directory_entry, file.c_str(), cluster);

    //Write back the parent directory cluster
    if(!write_sectors(disk, cluster_lba(cluster_number.second), fat_bs->sectors_per_cluster, directory_cluster.get())){
        return false;
    }

    //This cluster is the end of the chain
    if(!write_fat_value(disk, cluster, 0x0FFFFFF8)){
        return false;
    }

    //One cluster is now used for the directory entries
    fat_is->free_clusters -= 1;
    if(!write_is(disk, partition)){
        return false;
    }

    return true;
}
