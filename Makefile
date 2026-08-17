###############################################################################
# @file    Makefile
# @author  AntCode42
# @date    2026-07-10
# @brief   Build rules for the synchronome pipeline (CAPTURE/CONVERT/DIFF/WRITE).
# @details Compiles each pipeline stage as a separate translation unit
#          (init.c, uninit.c, thread_capture.c, thread_convert.c,
#          thread_write.c, seqgen.c, diff.c, main.c), all sharing
#          synchronome.h. Links against pthread (SCHED_FIFO threads),
#          rt (POSIX timers/message queues), required for pthread_create,
#          sem_*, timer_create/timer_settime, and mq_* once the queues
#          are wired in.
###############################################################################

# ====== CONFIG ======

NAME		= synchronome

CC			= gcc
CFLAGS		= -O3 -g -Wcpp
RM			= rm -f
LIBS		= -lrt -lpthread

SRC_DIR		= src
OBJ_DIR		= obj
INC_DIR		= include

SRC			= $(shell find $(SRC_DIR) -name "*.c")
OBJ			= $(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

INCLUDES	= -I $(INC_DIR)

.DEFAULT_GOAL := all

# ====== RULES ======

all: $(NAME)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LIBS) -o $(NAME)
	@echo "$(NAME) compiled successfully ✔"

clean:
	$(RM) -r $(OBJ_DIR)

fclean: clean
	$(RM) $(NAME)

re: fclean all

.PHONY: all clean fclean re