//xsh.c
#include "../../lib/common.h"
//#include "../drivers/vga.h"
#include "../../lib/unistd.h"
#include "../../fs/fs.h"
//#define LINUX

void putchar(char c) {
    write(1, &c, 1);
}

void puts(const char *s) {
    write(1, s, strlen(s));
    putchar('\n');
}

void _start(){
	
	puts("\nWelcome to X OS/Shell 0.0.2");
	puts("\nType \"cat\" to view file and \"echo\" to print text.");
	char buffer[1024]; //= "Hello";
	for(;;){
	
	char* tokens[64];
	
	//while(strcmp(tokens[0], "exit")!=0){
	write(1, "\nX ", 3);
	read(0, buffer, 1024);
	
	int position = 0;
	char *token = strtok(buffer, " \n\r\t");
    while(token)
    {
		tokens[position] = token;
		position++;
        //monitor_write(token);
        token = strtok(0, " \n\r\t");
    }
	if (position == 0) continue;   /* empty line */
	if(strcmp(tokens[0], "echo")==0){
		write(1, "\n", 1);
		for(int i = 1; i < position; i++){
		    write(1,tokens[i], strlen(tokens[i]));
			//monitor_put(' ');
			write(1, " ", 1);
		}
	}
	else if(strcmp(tokens[0], "cat")==0){
		#ifdef LINUX
			tokens[1][strlen(tokens[1]) - 1] = '\0';
		#endif
		//fs_node_t *fsnode = finddir_fs(fs_root, tokens[1]);
		int fd = open(tokens[1], 0,0);
		
		if(/*fsnode != 0*/fd!=-1){
			char buf[256];
        	//u32int sz = read_fs(fsnode, 0, 256, buf);
        	u32int sz = read(fd, buf, 256);
			//monitor_write_dec(sz);
			    write(1, "\n", 1);  // Важно: \n перед выводом
			write(1, buf, sz);puts("\n");//monitor_write("\n");
		}else{puts("\ncat: File not found.");}
	}
	else if(strcmp(tokens[0], "rm")==0){
		unlink(tokens[1]);
	}
	else if(strcmp(tokens[0], "ex") == 0){
            write(1, "\nStarting program: ", 20);
            write(1, tokens[1], strlen(tokens[1]));
            write(1, "\n", 1);
            
            //exec(tokens[1]);  // Теперь не блокирует навсегда
              exec(tokens[1], NULL,NULL);  // Теперь не блокирует навсегда
            write(1, "\nProgram finished. Back to shell.\n", 36);
        }
	else if(strcmp(tokens[0], "ls") == 0){
            int fd = open(tokens[1] ? tokens[1] : ".", 0, 0);
            if (fd < 0) continue;
            
            char buf[1024];
            int n = getdents(fd, buf, sizeof(buf));
            if (n <= 0) continue;
            
            int pos = 0;
            write(1, "\n", 1);
            while (pos < n) {
                struct linux_dirent *d = (struct linux_dirent *)(buf + pos);
                puts(d->d_name);
                pos += d->d_reclen;
            }
        }
     else if(strcmp(tokens[0], "exit") == 0) {
            puts("\nGoodbye!");
            exit(0);
        }
    /*else if (strcmp(tokens[0], "shutdown") == 0) {
        outw(0xB004, 0x2000);
        outw(0x604, 0x2000);
        outw(0x4004, 0x3400);
        outw(0x600, 0x34);
    }
     else if (strcmp(tokens[0], "startui") == 0) {
        vga_init();
        vga_clear(1);
    }*/
	else{
	//monitor_write("\nxsh: Command not found.");
		write(1, "\nxsh: Command not found.",25);
	}
	/*else if(strcmp(tokens[0], "date")==0){
		
	}*/
	
	
}
}