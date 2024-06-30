        //--------------------------------------------------------------------------------------------------
// System Programming                         I/O Lab                                   Spring 2024
//
/// @file
/// @brief resursively traverse directory tree and list all entries
/// @author Jiwon Kim
/// @studid 2019-11563
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <grp.h>
#include <pwd.h>

#define MAX_DIR 64            ///< maximum number of supported directories

/// @brief output control flags
#define F_DIRONLY   0x1       ///< turn on direcetory only option
#define F_SUMMARY   0x2       ///< enable summary
#define F_VERBOSE   0x4       ///< turn on verbose mode

/// @brief struct holding the summary
struct summary {
  unsigned int dirs;          ///< number of directories encountered
  unsigned int files;         ///< number of files
  unsigned int links;         ///< number of links
  unsigned int fifos;         ///< number of pipes
  unsigned int socks;         ///< number of sockets

  unsigned long long size;    ///< total size (in bytes)
};


/// @brief abort the program with EXIT_FAILURE and an optional error message
///
/// @param msg optional error message or NULL
void panic(const char *msg)
{
  if (msg) fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}


/// @brief read next directory entry from open directory 'dir'. Ignores '.' and '..' entries
///
/// @param dir open DIR* stream
/// @retval entry on success
/// @retval NULL on error or if there are no more entries
struct dirent *getNext(DIR *dir)
{
  struct dirent *next;
  int ignore;

  do {
    errno = 0;
    next = readdir(dir);
    if (errno != 0) perror(NULL);
    ignore = next && ((strcmp(next->d_name, ".") == 0) || (strcmp(next->d_name, "..") == 0));
  } while (next && ignore);

  return next;
}


/// @brief qsort comparator to sort directory entries. Sorted by name, directories first.
///
/// @param a pointer to first entry
/// @param b pointer to second entry
/// @retval -1 if a<b
/// @retval 0  if a==b
/// @retval 1  if a>b
static int dirent_compare(const void *a, const void *b)
{
  struct dirent *e1 = (struct dirent*)a;
  struct dirent *e2 = (struct dirent*)b;

  // if one of the entries is a directory, it comes first
  if (e1->d_type != e2->d_type) {
    if (e1->d_type == DT_DIR) return -1;
    if (e2->d_type == DT_DIR) return 1;
  }

  // otherwise sorty by name
  return strcmp(e1->d_name, e2->d_name);
}


char getFileTypeChar(mode_t mode) {
  switch (mode & S_IFMT) {
    case S_IFDIR: return 'd';
    case S_IFLNK: return 'l';
    case S_IFSOCK: return 's';
    case S_IFIFO: return 'f';
    case S_IFCHR: return 'c';
    case S_IFBLK: return 'b';
    default: return ' ';
  }
}

void updateStats(struct dirent *entries, size_t i, struct summary *stats) {
  switch(entries[i].d_type){
    case DT_DIR:
      stats->dirs ++;
      break;
    case DT_LNK:
      stats->links ++;
      break;
    case DT_FIFO:
      stats->fifos ++;
      break;
    case DT_SOCK:
      stats->socks ++;
      break;
    default:
      stats->files ++;
      break;
  }
}

int printEntryDetails(const char* path, struct stat* st, struct summary *stats, unsigned int flags) {
  char username[32];
  char groupname[32];
  char type;

  //get username
  struct passwd *pw = getpwuid(st->st_uid);
  if (pw == NULL) {
      printf("ERROR: getpwuid failed\n");
      return 1; //error - continue
  } else {
      strncpy(username, pw->pw_name, 8);
  }

  //get groupname
  struct group *gr = getgrgid(st->st_gid);
  if (gr == NULL) {
      printf("ERROR: getgrgid failed\n");
      return 1; //error - continue
  } else {
      strncpy(groupname, gr->gr_name, 8);
  }

  //get type
  type = getFileTypeChar(st->st_mode);

  //print details (VERBOSE mode)
  if(((flags & F_VERBOSE) != 0)){
    //get permissions
    char perms[10] = ""; 

    // User permissions
    strcat(perms, (st->st_mode & S_IRUSR) ? "r" : "-");
    strcat(perms, (st->st_mode & S_IWUSR) ? "w" : "-");
    strcat(perms, (st->st_mode & S_IXUSR) ? "x" : "-");

    // Group permissions
    strcat(perms, (st->st_mode & S_IRGRP) ? "r" : "-");
    strcat(perms, (st->st_mode & S_IWGRP) ? "w" : "-");
    strcat(perms, (st->st_mode & S_IXGRP) ? "x" : "-");

    // Other permissions
    strcat(perms, (st->st_mode & S_IROTH) ? "r" : "-");
    strcat(perms, (st->st_mode & S_IWOTH) ? "w" : "-");
    strcat(perms, (st->st_mode & S_IXOTH) ? "x" : "-");

    if((strlen(pw->pw_name) > 8) && (strlen(gr->gr_name) > 8)){
        printf("%s:%s  %10ld %8s  %c\n", pw->pw_name, gr->gr_name, st->st_size, perms, type);
    } else if(strlen(pw->pw_name) > 8){
      printf("%s:%-8s  %10ld %8s  %c\n", pw->pw_name, groupname, st->st_size, perms, type);
    } else if(strlen(gr->gr_name) > 8){
      printf("%8s:%s  %10ld %8s  %c\n", username, gr->gr_name, st->st_size, perms, type);
    } else{
      printf("%8s:%-8s  %10ld %8s  %c\n", username, groupname, st->st_size, perms, type);
    }
  }
  stats->size += st->st_size;

  return 0; // complete without error
}



/// @brief recursively process directory @a dn and print its tree
///
/// @param dn absolute or relative path string
/// @param depth depth in directory tree
/// @param stats pointer to statistics
/// @param flags output control flags (F_*)
void processDir(const char *dn, unsigned int depth, struct summary *stats, unsigned int flags)
{
  // TODO

  // 1: open dir
  DIR *dir = opendir(dn);
  errno = 0;
  if (dir == NULL) {
    // print error message and return
    char errmsg[128]="  ";

    for (unsigned int j = 0; j < depth - 1; j++) {
      strncat(errmsg, "  ", 2);
    }

    switch(errno) {
      case EACCES:
        strcat(errmsg, "ERROR: Permission denied");
        break;
      case ENOENT:
        strcat(errmsg, "ERROR: Directory does not exist"); 
        break;
      case ENOTDIR:
        strcat(errmsg, "ERROR: Not a directory");
        break;
      default:
        strcat(errmsg, "ERROR: Permission denied");
    }

    printf("%s\n", errmsg);
    return;
  }

  // 2: save entries in an array
  struct dirent *entry;
  struct dirent *entries;

    // allocate memory for entries
  entries = malloc(200 * sizeof(struct dirent));
  if (entries == NULL) { 
    printf("ERROR: Memory allocation failed\n");
    return;
  }

  errno = 0;
  size_t n = 0;

  while((entry = getNext(dir)) != NULL){
    entries[n++] = *entry;  //actual dirent struct stored in array

    if((n % 200) == 0){ // entries full, realloc
      entries = realloc(entries, (n+200) * sizeof(struct dirent));
      if(entries == NULL){
        printf("ERROR: Memory allocation failed\n");
        return;
      }
    }
  }

  closedir(dir);

  // 3: sort entries
  qsort(entries, n, sizeof(struct dirent), dirent_compare);

  // 4: print details based on flags
    // for each entry in the for loop
    // update stats
    // print details based on flags
        // For `-v` flag, include user:group, size, permissions, and type
        // For `-d` flag, skip non-directory entries
        // For `-s` flag, print summary of the directory
    // recursively call processDir if the entry is a directory

  for (size_t i=0; i<n; i++) {

    // 1) update stats
    updateStats(entries, i, stats);

      // if directory mode & not a directory, skip
    int IS_DIR = 0; // 1 for directory, 0 for file
    if (entries[i].d_type == DT_DIR) {
      IS_DIR = 1;
    }
    if ((flags & F_DIRONLY) != 0 && IS_DIR==0 ) {  
      continue;
    }

    // 2) name formatting
    char name[256]="  ";
    
      // add spaces for indentation
    for (unsigned int j = 0; j < depth-1; j++) {
      strncat(name, "  ", 2);
    }

      // print 
    if ((flags & F_VERBOSE) != 0) {
      // verbose mode
      if((strlen(name) + strlen(entries[i].d_name)) > 54){
        strncat(name, entries[i].d_name, 51-strlen(name));
        strncat(name, "...", 3);
      } else{
        strncat(name, entries[i].d_name, 54);
      }
      printf("%-54s  ", name); 

    } else {
      // non-verbose mode (long name)
      strncat(name, entries[i].d_name, 255 - strlen(name));
      printf("%s\n", name);
    }

    // 3) details (verbose/summary mode)
    if (((flags & F_VERBOSE) != 0) || ((flags & F_SUMMARY) != 0)) {
      struct stat st;
      
      // get path
      char path[256];
      strcpy(path, dn);
      strcat(path, "/");
      strcat(path, entries[i].d_name);

      if (lstat(path, &st) < 0) { //error
        if ((flags & F_VERBOSE) != 0) {
          printf("Permission denied\n");
        }
        continue;
      }

      // print metadata details
      if (((flags & F_VERBOSE)!= 0 || (flags & F_SUMMARY) != 0)){
        int error = 0;
        error = printEntryDetails(path, &st, stats, flags);
        if (error == 1) {
          continue;
        }
      }
    }
    // 4) recursive call of processDir
    if (entries[i].d_type == DT_DIR) {
      char path[256];
      strcpy(path, dn);
      strcat(path, "/");
      strcat(path, entries[i].d_name);
      processDir(path, depth + 1, stats, flags);
    }
  }
  // 5: free memory
  free(entries);
}


/// @brief print program syntax and an optional error message. Aborts the program with EXIT_FAILURE
///
/// @param argv0 command line argument 0 (executable)
/// @param error optional error (format) string (printf format) or NULL
/// @param ... parameter to the error format string
void syntax(const char *argv0, const char *error, ...)
{
  if (error) {
    va_list ap;

    va_start(ap, error);
    vfprintf(stderr, error, ap);
    va_end(ap);

    printf("\n\n");
  }

  assert(argv0 != NULL);

  fprintf(stderr, "Usage %s [-d] [-s] [-v] [-h] [path...]\n"
                  "Gather information about directory trees. If no path is given, the current directory\n"
                  "is analyzed.\n"
                  "\n"
                  "Options:\n"
                  " -d        print directories only\n"
                  " -s        print summary of directories (total number of files, total file size, etc)\n"
                  " -v        print detailed information for each file. Turns on tree view.\n"
                  " -h        print this help\n"
                  " path...   list of space-separated paths (max %d). Default is the current directory.\n",
                  basename(argv0), MAX_DIR);

  exit(EXIT_FAILURE);
}


/// @brief program entry point
int main(int argc, char *argv[])
{
  //
  // default directory is the current directory (".")
  //
  const char CURDIR[] = ".";
  const char *directories[MAX_DIR];
  int   ndir = 0;

  struct summary tstat;
  struct summary dstat;
  unsigned int flags = 0;

  //
  // parse arguments
  //
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // format: "-<flag>"
      if      (!strcmp(argv[i], "-d")) flags |= F_DIRONLY;
      else if (!strcmp(argv[i], "-s")) flags |= F_SUMMARY;
      else if (!strcmp(argv[i], "-v")) flags |= F_VERBOSE;
      else if (!strcmp(argv[i], "-h")) syntax(argv[0], NULL);
      else syntax(argv[0], "Unrecognized option '%s'.", argv[i]);
    } else {
      // anything else is recognized as a directory
      if (ndir < MAX_DIR) {
        directories[ndir++] = argv[i];
      } else {
        printf("Warning: maximum number of directories exceeded, ignoring '%s'.\n", argv[i]);
      }
    }
  }

  // if no directory was specified, use the current directory
  if (ndir == 0) directories[ndir++] = CURDIR;


  //
  // process each directory
  //
  // TODO
  //
  // Pseudo-code
  // - reset statistics (tstat)
  // - loop over all entries in 'directories' (number of entires stored in 'ndir')
  //   - reset statistics (dstat)
  //   - if F_SUMMARY flag set: print header
  //   - print directory name
  //   - call processDir() for the directory
  //   - if F_SUMMARY flag set: print summary & update statistics
  
  memset(&tstat, 0, sizeof(tstat));
  
  //... starting here
  for (int i = 0; i < ndir; i++){
    // 1) reset statistics (dstat)
    memset(&dstat, 0, sizeof(dstat));
    
    // 2) print header
    if((flags & F_SUMMARY) != 0){
      if((flags & F_VERBOSE) != 0){
        // summary & verbose mode
        printf("%-54s  %8s:%-8s  %10s  %8s %-4s \n", "Name", "User", "Group", "Size", "Perms", "Type");
      } else{
        // summary mode
        printf("Name\n");
      }

      for(int i=0; i<100; i++) {
        printf("-");
      }
      printf("\n");
    }
    
    // 3) print directory name
    printf("%s\n", directories[i]);

    // 4) call processDir() for the directory
    processDir(directories[i], 1, &dstat, flags);
    
    // 5) print summary of each directory
    if((flags & F_SUMMARY) != 0) {

      printf("----------------------------------------------------------------------------------------------------\n");

      char summary[128]="";

      // dir only mode
      if((flags & F_DIRONLY) != 0) {
        
        if(dstat.dirs == 1){
          printf("1 directory\n\n");
        } else{
          printf("%d directories\n\n", dstat.dirs);
        }
        continue;
      }

      if(dstat.files == 1){
        strcat(summary, "1 file, ");
      } else{
        strcat(summary, dstat.files);
        strcat(summary, " files, ");
      }
      if(dstat.dirs == 1){
        strcat(summary, "1 directory, ");
      } else{
        strcat(summary, dstat.dirs);
        strcat(summary, " directories, ");
      }
      if(dstat.links == 1){
        strcat(summary, "1 link, ");
      } else{
        strcat(summary, dstat.links);
        strcat(summary, " links, ");
      }
      if(dstat.fifos == 1){
        strcat(summary, "1 pipe, and ");
      } else{
        strcat(summary, dstat.fifos);
        strcat(summary, " pipes, and ");
      }
      if(dstat.socks == 1){
        strcat(summary, "1 socket");
      } else{
        strcat(summary, dstat.socks);
        strcat(summary, " sockets");
      }
    
      if((flags & F_VERBOSE) != 0){
        if(strlen(summary) < 69){
          printf("%-68s   %14llu\n\n", summary, dstat.size);
        } else{
          char newsum[128];
          strncpy(newsum, summary, 65);
          strcat(newsum, "...");
          printf("%-68s   %14llu\n\n", newsum, dstat.size);
        }
      } else{
        printf("%s\n\n", summary);
        }


      tstat.dirs += dstat.dirs;
      tstat.fifos += dstat.fifos;
      tstat.files += dstat.files;
      tstat.links += dstat.links;
      tstat.size += dstat.size;
      tstat.socks += dstat.socks;
    }

  }

  //
  // print grand total
  //
  if ((flags & F_SUMMARY) && (ndir > 1)) {
    if (flags & F_DIRONLY) {
      printf("Analyzed %d directories:\n"
             "  total # of directories:  %16d\n",
             ndir, tstat.dirs);
    } else {
      printf("Analyzed %d directories:\n"
             "  total # of files:        %16d\n"
             "  total # of directories:  %16d\n"
             "  total # of links:        %16d\n"
             "  total # of pipes:        %16d\n"
             "  total # of sockets:      %16d\n",
             ndir, tstat.files, tstat.dirs, tstat.links, tstat.fifos, tstat.socks);

      if (flags & F_VERBOSE) {
        printf("  total file size:         %16llu\n", tstat.size);
      }
    }
  }

  //
  // that's all, folks!
  //
  return EXIT_SUCCESS;
}
