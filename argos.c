#include <argp.h>
#include <stdbool.h> // bool, true, false
#include <stdint.h> // size_t
#include <stdlib.h> // malloc(), calloc(), realloc(), free()
#include <signal.h> // signal(), SIGINT
#include <string.h> // strerror()
#include <errno.h> // errno()
#include <unistd.h>
#include <sys/inotify.h>
#include <soph/ll.h> // sll_t, sll_push_head()


#define eprintf(FMT, ...) (fprintf(stderr, FMT, ##__VA_ARGS__))
#define eflush() (fflush(stderr))
#define error_noexit(FMT, ...) ({ errno ? eprintf("argos: " FMT ": %s\n", ##__VA_ARGS__, strerror(errno)) : eprintf("argos: " FMT "\n", ##__VA_ARGS__); })
#define error(FMT, ...) ({ error_noexit(FMT, ##__VA_ARGS__); exit(EXIT_FAILURE); })


const char *argp_program_version = "argos 0.1.1";
const char *argp_program_bug_address = 0;
static char doc[] = "Wait for events from FILE(s) and run a given command.";
static char args_doc[] = "[-X <COMMAND>] [-ABCDMOS <COMMAND>] <FILE...>";
static struct argp_option options[] = {
	{ 0, 0, 0, OPTION_DOC, "Usage and help:", -1 },
		{ 0,           'h', 0, OPTION_HIDDEN, 0, -1 },

	{ 0, 0, 0, OPTION_DOC, "Control output of argos:", 1 },
		{ "pretty",    'p', 0, 0, "Produce pretty output.",             1 },
		{ "verbose",   'v', 0, 0, "Produce verbose output.",            1 },
		{ "quiet-out", 'Q', 0, 0, "Silence the output of subcommands.", 1 },
		{ "quiet",     'q', 0, 0, "Produce no output.",                 1 },
		{ "silent",    's', 0, OPTION_ALIAS, 0,                         1 },

	{ 0, 0, 0, OPTION_DOC, "Configure inotify responses:", 2 },
		{ "ALL",       'X', 0, 0, "Run COMMAND when FILE fires any event.",     2 },
		{ "ACCESS",    'A', 0, 0, "Run COMMAND when FILE is accessed.",         2 },
		{ "MODIFY",    'M', 0, 0, "Run COMMAND when FILE is modified.",         2 },
		{ "OPEN",      'O', 0, 0, "Run COMMAND when FILE is opened.",           2 },
		{ "CREATE",    'C', 0, 0, "Run COMMAND when FILE is created.",          2 },
		{ "CLOSE",     'S', 0, 0, "Run COMMAND when FILE is closed.",           2 },
		{ "DELETE",    'D', 0, 0, "Run COMMAND when FILE is deleted.",          2 },
		{ "ATTRIB",    'B', 0, 0, "Run COMMAND when FILE's attributes change.", 2 },

	{ 0 }
};

#define PUSH_ACCESS (1 << 0)
#define PUSH_MODIFY (1 << 1)
#define PUSH_OPEN   (1 << 2)
#define PUSH_CREATE (1 << 3)
#define PUSH_CLOSE  (1 << 4)
#define PUSH_DELETE (1 << 5)
#define PUSH_ATTRIB (1 << 6)

typedef struct {
	char **files;
	size_t files_len;
	size_t files_size;

	unsigned push_to;

	sll_t cmd_all;
	sll_t cmd_access;
	sll_t cmd_modify;
	sll_t cmd_open;
	sll_t cmd_create;
	sll_t cmd_close;
	sll_t cmd_delete;
	sll_t cmd_attrib;

	bool is_verbose;
	bool is_quiet;
	bool is_command_quiet;
	bool is_pretty;
	bool is_forced;
} args_t;

args_t args;

static error_t parse_opt(int key, char *arg, struct argp_state *state);
static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };

#define ANSI_RESET "\e[m"
#define CLEAR_SCREEN "\e[2J\e[3J\e[H"

#define EVENT_PREFIX ""
#define EVENT(X) EVENT_PREFIX X " "
#define EVENT_PREFIX_COLOR ""
#define EVENT_COLOR "\e[32m"
#define EVENT_PRETTY(X) EVENT_PREFIX_COLOR EVENT_PREFIX EVENT_COLOR X ANSI_RESET " "

#define COMMAND_PREFIX "  $ "
#define COMMAND(X) COMMAND_PREFIX X "\n"
#define COMMAND_PREFIX_COLOR "\e[34m"
#define COMMAND_COLOR ""
#define COMMAND_PRETTY(X) COMMAND_PREFIX_COLOR COMMAND_PREFIX COMMAND_COLOR X ANSI_RESET "\n"

#define OUTPUT_PREFIX "    -> "
#define OUTPUT_SILENT(X) X "\n"
#define OUTPUT(X) OUTPUT_PREFIX X "\n"
#define OUTPUT_PREFIX_COLOR "\e[90m"
#define OUTPUT_COLOR ANSI_RESET
#define OUTPUT_PRETTY(X) OUTPUT_PREFIX_COLOR OUTPUT_PREFIX OUTPUT_COLOR X ANSI_RESET "\n"

// pretty print the "EVENT: DIR FILE" line
void print_event(const char *name, char *dir, char *file) {
	if (args.is_quiet) return;
	eprintf(args.is_pretty ? EVENT_PRETTY("%s") : EVENT("%s"), name);
	dir ? eprintf("%s %s\n", dir, file) : eprintf("%s\n", file);
	eflush();
}

// pretty print the "$ COMMAND" line
void print_command(char *cmd) {
	if (args.is_quiet) return;
	eprintf(args.is_pretty ? COMMAND_PRETTY("%s") : COMMAND("%s"), cmd);
	eflush();
}

// pretty print the "-> line of stdout" line
void print_output_line(char *line, size_t len) {
	if (args.is_command_quiet) return;

	if (args.is_quiet)
		printf(OUTPUT_SILENT("%.*s"), len, line);
	else if (args.is_pretty)
		printf(OUTPUT_PRETTY("%.*s"), len, line);
	else
		printf(OUTPUT("%.*s"), len, line);

	fflush(stdout);
}

// size of chunks read from the stdout of subcommands
#define STDOUT_CHUNK_SIZE 4096

// pretty print an event and run its handlers
void run_event(const char *name, char *dir, char *file, sll_t *commands) {
	print_event(name, dir, file);

	// sets $event
	setenv("event", name, 1);

	FILE *fp;
	size_t size, num_read;
	char *buf, *ptr;

	sll_node_t *node = commands->head;
	while (node) {
		print_command((char *) node->val);

		// open a pipe that runs the given command
		if ((fp = popen((char *) node->val, "r")) == 0) {
			eprintf("error: popen: %s\n", strerror(errno));
			node = node->next;
			continue;
		}

		// read the output of the command into a growable buffer
		size = 0;
		buf = malloc(STDOUT_CHUNK_SIZE * sizeof(char));
		for (;;) {
			num_read = fread(buf + size, sizeof(char), STDOUT_CHUNK_SIZE, fp);
			size += num_read;
			if (num_read < STDOUT_CHUNK_SIZE) break;
			buf = realloc(buf, (size + STDOUT_CHUNK_SIZE) * sizeof(char));
		}

		// print each line with special formatting
		// loops through the string until it knows it has a full line, then prints it
		char *line_start = buf, *line_end = line_start;
		while (line_start < buf + size) {
			if (*line_end++ != '\n') continue;
			if (line_end - line_start > 1)
				print_output_line(line_start, line_end - line_start - 1);
			line_start = line_end;
		}

		free(buf);
		node = node->next;
	}
}

void handle_event(struct inotify_event *i) {
	// if no event, don't do anything
	if (!i->mask) return;

	// get the dirname and the filename from the event
	char *dir = i->len ? args.files[i->wd - 1] : NULL;
	char *file = i->len ? i->name : args.files[i->wd - 1];

	// set $file and $dir accordingly (or unset them if file or dir are null)
	if (file) setenv("file", file, 1); else unsetenv("file");
	if (dir) setenv("dir", dir, 1); else unsetenv("dir");

	// run any -X commands with the $event variable properly set
	if (args.cmd_all.len) {
		if (i->mask & IN_ACCESS)        run_event("ACCESS", dir, file, &args.cmd_all);
		if (i->mask & IN_ATTRIB)        run_event("ATTRIB", dir, file, &args.cmd_all);
		if (i->mask & IN_CLOSE_NOWRITE) run_event("CLOSE_NOWRITE", dir, file, &args.cmd_all);
		if (i->mask & IN_CLOSE_WRITE)   run_event("CLOSE_WRITE", dir, file, &args.cmd_all);
		if (i->mask & IN_CREATE)        run_event("CREATE", dir, file, &args.cmd_all);
		if (i->mask & IN_DELETE)        run_event("DELETE", dir, file, &args.cmd_all);
		if (i->mask & IN_DELETE_SELF)   run_event("DELETE_SELF", dir, file, &args.cmd_all);
		if (i->mask & IN_IGNORED)       run_event("IGNORED", dir, file, &args.cmd_all);
		if (i->mask & IN_ISDIR)         run_event("ISDIR", dir, file, &args.cmd_all);
		if (i->mask & IN_MODIFY)        run_event("MODIFY", dir, file, &args.cmd_all);
		if (i->mask & IN_MOVE_SELF)     run_event("MOVE_SELF", dir, file, &args.cmd_all);
		if (i->mask & IN_MOVED_FROM)    run_event("MOVED_FROM", dir, file, &args.cmd_all);
		if (i->mask & IN_MOVED_TO)      run_event("MOVED_TO", dir, file, &args.cmd_all);
		if (i->mask & IN_OPEN)          run_event("OPEN", dir, file, &args.cmd_all);
		if (i->mask & IN_Q_OVERFLOW)    run_event("Q_OVERFLOW", dir, file, &args.cmd_all);
		if (i->mask & IN_UNMOUNT)       run_event("UNMOUNT", dir, file, &args.cmd_all);
	}

	// run any commands for specific events
	if (args.cmd_access.len && i->mask & IN_ACCESS)
		run_event("ACCESS", dir, file, &args.cmd_access);
	if (args.cmd_modify.len && i->mask & IN_MODIFY)
		run_event("MODIFY", dir, file, &args.cmd_modify);
	if (args.cmd_close.len && (i->mask & IN_CLOSE_NOWRITE || i->mask & IN_CLOSE_WRITE))
		run_event("CLOSE", dir, file, &args.cmd_close);
	if (args.cmd_open.len && i->mask & IN_OPEN)
		run_event("OPEN", dir, file, &args.cmd_open);
	if (args.cmd_create.len && i->mask & IN_CREATE)
		run_event("CREATE", dir, file, &args.cmd_create);
	if (args.cmd_delete.len && i->mask & IN_DELETE)
		run_event("DELETE", dir, file, &args.cmd_delete);
	if (args.cmd_attrib.len && i->mask & IN_ATTRIB)
		run_event("ATTRIB", dir, file, &args.cmd_attrib);
}

// global inotify file descriptor
int inotify_fd;

void exit_handler() {
	// remember to close the inotify file descriptor before exiting
	if (inotify_fd) close(inotify_fd);

	// clean up memory allocations
	free(args.files);
	sll_free(&args.cmd_all);
	sll_free(&args.cmd_access);
	sll_free(&args.cmd_modify);
	sll_free(&args.cmd_open);
	sll_free(&args.cmd_close);
	sll_free(&args.cmd_create);
	sll_free(&args.cmd_delete);
	sll_free(&args.cmd_attrib);
}

void sigint_handler(int signum) {
	exit_handler();
	exit(EXIT_SUCCESS);
}

// how long to make the buffer that holds inotify events
#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

int main(int argc, char *argv[]) {
	if (atexit(exit_handler)) error("failed to register exit handler");
	if (signal(SIGINT, sigint_handler)) error("failed to register SIGINT handler");

	// buffer to hold inotify events
	char buf[BUF_LEN] __attribute__ ((aligned(8)));

	args = (args_t) {
		.files = malloc(4096),
		.files_size = 4096,
		.files_len = 0,

		.push_to = 0,

		.cmd_all = sll_new(),
		.cmd_access = sll_new(),
		.cmd_modify = sll_new(),
		.cmd_open = sll_new(),
		.cmd_close = sll_new(),
		.cmd_create = sll_new(),
		.cmd_delete = sll_new(),
		.cmd_attrib = sll_new(),
	};

	argp_parse(&argp, argc, argv, ARGP_IN_ORDER, 0, 0);

	if ((inotify_fd = inotify_init()) == -1) error("inotify_init");

	char *file, *end;
	int wd;

	for (size_t i = 0; i < args.files_len; i++) {
		if ((end = file = args.files[i]) == 0) continue;

		// if the file cannot be accessed, throw an error (unless -f is specified)
		if (!args.is_forced && access(file, F_OK))
			error("%s", file);

		// tell inotify to watch the given file for all events
		wd = inotify_add_watch(inotify_fd, file, IN_ALL_EVENTS);
		if (!wd) error("inotify_add_watch");

		// this program assumes that a file's watch descriptor is one more than its index into args.files
		if (wd != i + 1) error("inotify_add_watch returned an invalid watch descriptor");
	}

	ssize_t num_read;
	struct inotify_event *event;
	char *ptr;

	// event loop
	for (;;) {
		num_read = read(inotify_fd, buf, BUF_LEN);
		if (num_read == 0) error("read() from inotify fd returned 0!");
		if (num_read == -1) error("read");

		// read(inotify_fd) can give multiple events per call, so carve them out
		for (ptr = buf; ptr < buf + num_read; ) {
			event = (struct inotify_event *) ptr;
			handle_event(event);
			ptr += sizeof(struct inotify_event) + event->len;
		}
	}
}


// argp parser
static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	switch (key) {
		// generic flags
		case 'h': argp_state_help(state, state->out_stream, ARGP_HELP_STD_HELP); break;
		case 'v': args.is_verbose = true; break;
		case 'q': args.is_quiet = true; break;
		case 'Q': args.is_command_quiet = true; break;
		case 'p': args.is_pretty = true; break;
		case 'f': args.is_forced = true; break;

		// event flags
		case 'X': sll_push_head(&args.cmd_all, arg); break;
		case 'A': args.push_to |= PUSH_ACCESS; break;
		case 'M': args.push_to |= PUSH_MODIFY; break;
		case 'O': args.push_to |= PUSH_OPEN;   break;
		case 'C': args.push_to |= PUSH_CREATE; break;
		case 'S': args.push_to |= PUSH_CLOSE;  break;
		case 'D': args.push_to |= PUSH_DELETE; break;
		case 'B': args.push_to |= PUSH_ATTRIB; break;

		case ARGP_KEY_ARG:
			// flag right before this arg was an event flag, so this arg must be a command
			if (args.push_to) {
				if (args.push_to & PUSH_ACCESS) sll_push_head(&args.cmd_access, arg);
				if (args.push_to & PUSH_MODIFY) sll_push_head(&args.cmd_modify, arg);
				if (args.push_to & PUSH_OPEN)   sll_push_head(&args.cmd_open, arg);
				if (args.push_to & PUSH_CREATE) sll_push_head(&args.cmd_create, arg);
				if (args.push_to & PUSH_CLOSE)  sll_push_head(&args.cmd_close, arg);
				if (args.push_to & PUSH_DELETE) sll_push_head(&args.cmd_delete, arg);
				if (args.push_to & PUSH_ATTRIB) sll_push_head(&args.cmd_attrib, arg);
				args.push_to = 0;
				break;
			}

			// argument that isn't a flag, therefore it must be a filename
			if (args.files_len >= args.files_size) {
				args.files = realloc(args.files, args.files_size *= 2);
				if (!args.files) error("failed to reallocate filename list");
			}
			args.files[args.files_len++] = arg;
			break;
	    case ARGP_KEY_END:
			if (args.files_len == 0) {
		    	error_noexit("not enough arguments");
				argp_usage(state);
			}
			break;
		default: return ARGP_ERR_UNKNOWN;
	}
    return 0;
}

