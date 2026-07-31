#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// 正则匹配函数（从 grep.c 移植）
int matchhere(char *re, char *text);
int star(int c, char *re, char *text);

int match(char *re, char *text) {
    if (re[0] == '^')
        return matchhere(re + 1, text);
    do {
        if (matchhere(re, text))
            return 1;
    } while (*text++ != '\0');
    return 0;
}

int matchhere(char *re, char *text) {
    if (re[0] == '\0')
        return 1;
    if (re[1] == '*')
        return star(re[0], re + 2, text);
    if (re[0] == '$' && re[1] == '\0')
        return *text == '\0';
    if (*text != '\0' && (re[0] == '.' || re[0] == *text))
        return matchhere(re + 1, text + 1);
    return 0;
}

int star(int c, char *re, char *text) {
    do {
        if (matchhere(re, text))
            return 1;
    } while (*text != '\0' && (*text++ == c || c == '.'));
    return 0;
}

void find(char *path, char *target) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    // 1. 打开当前路径
    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    // 2. 获取路径状态，如果不是目录，直接返回
    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }
    if (st.type != T_DIR) {
        // 如果是文件，直接比较文件名（虽然传进来的 path 本身是目录，但递归时会用到）
        close(fd);
        return;
    }

    // 3. 如果路径长度超过缓冲区大小，报错
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
        fprintf(2, "find: path too long\n");
        close(fd);
        return;
    }

    // 4. 循环读取目录中的每个条目
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0)
            continue;

        // 【关键】跳过 "." 和 ".."，防止死循环
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        // 拼接完整路径：path + "/" + de.name
        memmove(buf, path, strlen(path));
        p = buf + strlen(path);
        *p++ = '/';
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;  // 确保字符串结尾

        // 获取该条目的状态（判断是文件还是目录）
        if (stat(buf, &st) < 0) {
            fprintf(2, "find: cannot stat %s\n", buf);
            continue;
        }

        if (st.type == T_DIR) {
            // 如果是目录，递归进入查找
            find(buf, target);
        }
        else {
            // 如果是文件，比较文件名是否匹配目标
            if (match(target, de.name)) {
                printf("%s\n", buf);
            }
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(2, "Usage: find <directory> <regex>\n");
        exit(1);
    }
    find(argv[1], argv[2]);
    exit(0);
}