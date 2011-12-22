all: vncshare

vncshare: main.c
	gcc -pthread -Wall -Wextra -o vncshare main.c
