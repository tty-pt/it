/* SPDX-FileCopyrightText: 2022 Paulo Andre Azevedo Quirino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * It is important that this program can be understood by people who are not
 * programmers, so I'm adding an in-depth description of the algorithm as
 * comments. The most important ones will be at the top of the functions. To
 * understand the algorithm, I recommend that you start from the bottom of this
 * file, and scroll up as needed. It might also be useful to lookup the
 * definition of specific functions if you want to know how they work in more
 * detail. For that it is enough that you search for "^process_start" for
 * example. It is also recommended that before you do that, you read the
 * README.md to understand the format of the input data files.
 *
 * Mind you, dates are expressed in ISO-8601 format to users, but internally
 * we use unix timestamps. This is to facilitate a user to analyse the input
 * data easily while permitting the software to evaluate datetimes
 * mathematically in a consistent way.
 *
 * Person ids are also particular in this way. In the input file they are
 * textual, but internally we use numeric ids to which they correspond.
 *
 * Currency values are read as float but internally they are integers.
 *
 * The general idea of the algorithm involves a few data structures:
 *
 * One of them is a weighted and directed graph, in which each node represents
 * a person, and the edges connecting the nodes represent the accumulated debt
 * between them.
 *
 * Another is a binary search tree (BST) that stores intervals of time, that
 * we query in order to find out who was present during the billing periods,
 * etc. Actually, there are two of these kinds of BSTs. One That only stores
 * intervals where the person is actually in the house (BST A), another that
 * stores intervals where the person is renting a room there, but might not be
 * present (BST B).
 *
 * Jump to the main function when you are ready to check out how it all works.
 *
 * Happy reading!
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE
/* #include <ctype.h> */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
/* #include <string.h> */
#include <fcntl.h>

/* #define USERNAME_MAX_LEN 32 */

unsigned g_len = 0;
unsigned g_notfound = (unsigned) -1;
unsigned pflags = 0;

static inline void
usage(char *prog)
{
	fprintf(stderr, "Usage: %s [-S PATH] [[-rs] QUERY...]\n", prog);
	fprintf(stderr, "    Options:\n");
	fprintf(stderr, "        -r QUERY  Only always present.\n");
	fprintf(stderr, "        -s QUERY  Show splits.\n");
	fprintf(stderr, "        -s PATH   Set socket path.\n");
}

/* The main function is the entry point to the application. In this case, it
 * is very basic. What it does is it reads each line that was fed in standard
 * input. This allows you to feed it any file you want by running:
 *
 * $ cat file.txt | ./it "2022-03-01 2022-05-15T10:00:00" "now" "2023-11-18"
 *
 * You can also just run "./it", input manually, and then hit ctrl+D.
 *
 * For each line read, it then calls process_line, with that line as an
 * argument (a pointer). Look at process_line right above this comment to
 * understand how it works.
 *
 * After reading each line in standard input, the program shows the debt
 * that was calculated, that is owed between the people (ge_show_all).
 */
int
main(int argc, char *argv[])
{
	char buf[BUFSIZ];
	char *line = NULL;
	char *sockpath = "/tmp/it-sock";
	ssize_t linelen;
	size_t linesize;
	struct sockaddr_un addr;
	int sock;
	char c;

	while ((c = getopt(argc, argv, "r:s:S:")) != -1) switch (c) {
		case 'r':
		case 's': break;
		case 'S':
			  sockpath = optarg;
			  break;
		default:
			  usage(*argv);
			  return 1;
	}

	optind = 0;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sock == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sockpath);

	if (connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1) {
		perror("connect");
		close(sock);
		exit(EXIT_FAILURE);
	}

	if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1) {
		perror("Failed to set stdin to non-blocking");
		return 1;
	}

	while ((linelen = getline(&line, &linesize, stdin)) >= 0)
		write(sock, line, linelen);

	write(sock, "EOF\n", 4);
	free(line);

	while ((c = getopt(argc, argv, "r:s:S:")) != -1) switch (c) {
		case 'r':
			strcpy(buf, "+ ");
			strcat(buf, optarg);
			write(sock, buf, strlen(buf));
			read(sock, buf, sizeof(buf));
			printf("%s", buf);
			break;
		case 's':
			strcpy(buf, "* ");
			strcat(buf, optarg);
			write(sock, buf, strlen(buf));
			read(sock, buf, sizeof(buf));
			printf("%s", buf);
			break;
		default:
			usage(*argv);
			return 1;
	}

	while (optind < argc) {
		char *arg = argv[optind++];
		write(sock, arg, strlen(arg));
		read(sock, buf, sizeof(buf));
		printf("%s", buf);
	}

	return EXIT_SUCCESS;
}
