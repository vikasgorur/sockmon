all: sockmon

sockmon: sockmon.c
	gcc -o sockmon -g -O0 sockmon.c -lpthread

clean:
	rm -f sockmon *~