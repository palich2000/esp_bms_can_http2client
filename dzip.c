#include "dzip.h"
#include "dlog.h"

#include <zip.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>


int extract_zip(const char * zip_archive_filename, const char * dst_folder) {
    struct zip * zip_file;
    struct zip_file * file_in_zip;
    int err;
    int files_total;
    int r;
    char buffer[10000];
    struct stat st = {0};


    if ((!zip_archive_filename) || (!dst_folder)) {
        DLOG_ERR("Error: zip file name or dst folder is empty");
        return -1;
    }


    if (stat(dst_folder, &st) == -1) {
        if (mkdir(dst_folder, 0700)) {
            DLOG_ERR("Error: can't create destination dir %s : %s", dst_folder, strerror(errno));
            return -1;
        }
    }

    zip_file = zip_open(zip_archive_filename, 0, &err);
    if (!zip_file) {
        DLOG_ERR("Error: can't open file %s", zip_archive_filename);
        return -1;
    };

    files_total = zip_get_num_files(zip_file);

    bool write_error = false;

    for (int zip_fil_num = 0; ((zip_fil_num < files_total) && !write_error) ; zip_fil_num++) {
        struct zip_stat zs;
        if (zip_stat_index(zip_file, zip_fil_num, 0, &zs) == 0) {
            file_in_zip = zip_fopen_index(zip_file, zip_fil_num, 0);
            if (file_in_zip) {
                DLOG_INFO("extract %s ", zs.name);
                char dst_file_name[255] = {};
                snprintf(dst_file_name, sizeof(dst_file_name) - 1, "%s/%s", dst_folder, zs.name);
                int fd = open(dst_file_name, O_RDWR | O_CREAT | O_TRUNC);
                if (fd >= 0) {
                    while ( (r = zip_fread(file_in_zip, buffer, sizeof(buffer))) > 0) {

                        if (write(fd, buffer, r) != r) {
                            DLOG_ERR("write file %s error %s", dst_file_name, strerror(errno));
                            write_error = true;
                            break;
                        }
                    }
                    close(fd);
                    if (write_error) {
                        unlink(dst_file_name);
                    }
                } else {
                    DLOG_ERR("write create %s error %s", dst_file_name, strerror(errno));
                }
                zip_fclose(file_in_zip);
            } else {
                DLOG_ERR("Error: can't open file %s in zip", zs.name);
            }
        }
    }
    zip_close(zip_file);

    if (write_error)
        return(-1);
    else
        return(0);
};
