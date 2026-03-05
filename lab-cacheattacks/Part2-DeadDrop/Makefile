include ../cpu.mk

TARGETS=receiver sender receiver_osc sender_osc
UTILS=util.o 

all: $(TARGETS)

$(UTILS): %.o: %.c %.h
	$(CC) $(CFLAGS) -c $<

%.o: %.c util.h
	$(CC) $(CFLAGS)  -c $< 

$(TARGETS): %:%.o util.o
	$(CC) $(CFLAGS) $^ -o $@

run_sender: sender 
	taskset -c $(SENDER_CPU) ./sender

run_receiver: receiver
	taskset -c $(RECEIVER_CPU) ./receiver

run_sender_osc: sender_osc
	taskset -c $(SENDER_CPU) ./sender_osc

run_receiver_osc: receiver_osc
	taskset -c $(RECEIVER_CPU) ./receiver_osc

.PHONY:	clean

clean:
	$(RM) *.o $(HELPERS) $(TARGETS) 
