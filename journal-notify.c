/*
 * (C) 2014-2015 by Christian Hesse <mail@eworm.de>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include "journal-notify.h"

const char * program = NULL;

const static char optstring[] = "aehi:m:nor:t:v";
const static struct option options_long[] = {
	/* name			has_arg			flag	val */
	{ "and",		no_argument,		NULL,	'a' },
	{ "extended-regex",	no_argument,		NULL,	'e' },
	{ "help",		no_argument,		NULL,	'h' },
	{ "icon",		required_argument,	NULL,	'i' },
	{ "match",		required_argument,	NULL,	'm' },
	{ "no-case",		no_argument,		NULL,	'n' },
	{ "or",			no_argument,		NULL,	'o' },
	{ "regex",		required_argument,	NULL,	'r' },
	{ "timeout",		required_argument,	NULL,	't' },
	{ "verbose",		no_argument,		NULL,	'v' },
	{ 0, 0, 0, 0 }
};

/*** notify ***/
int notify(const char * summary, const char * body, const char * icon, int timeout) {
	NotifyNotification * notification;
	int rc = -1;

	notification =
#if NOTIFY_CHECK_VERSION(0, 7, 0)
		notify_notification_new(summary, body, icon);
#else
		notify_notification_new(summary, body, icon, NULL);
#endif

	if (notification == NULL)
		return rc;

	/* NOTIFY_EXPIRES_NEVER == 0 */
	if (timeout >= 0)
		notify_notification_set_timeout(notification, timeout * 1000);

	if (notify_notification_show(notification, NULL) == FALSE)
		goto out;

	rc = 0;

out:
	g_object_unref(G_OBJECT(notification));

	return rc;
}

/*** main ***/
int main(int argc, char **argv) {
	int i, rc = EXIT_FAILURE;
	uint8_t verbose = 0;

	uint8_t have_regex = 0;
	regex_t regex;
	int regex_flags = REG_NOSUB;

	sd_journal * journal;
	const void * data;
	size_t length;

	char * summary, * message;
	const char * icon = DEFAULTICON;
	int timeout = -1;

	program = argv[0];

	/* get command line options - part I
	 * just get -h (help), -e and -n (regex options) here */
	while ((i = getopt_long(argc, argv, optstring, options_long, NULL)) != -1) {
		switch (i) {
			case 'e':
				regex_flags |= REG_EXTENDED;
				break;
			case 'h':
				fprintf(stderr, "usage: %s [-e] [-h] [-i ICON] [-m MATCH] [-o -m MATCH] [-a -m MATCH] [-n] [-r REGEX] [-t SECONDS] [-vv]\n", program);
				return EXIT_SUCCESS;
			case 'n':
				regex_flags |= REG_ICASE;
				break;
			case 'v':
				verbose++;
				break;
		}
	}

	/* say hello */
	if (verbose > 0)
		printf("%s v%s (compiled: " __DATE__ ", " __TIME__ ")\n", program, VERSION);

	/* open journal */
	if ((rc = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY + SD_JOURNAL_SYSTEM)) < 0) {
		fprintf(stderr, "Failed to open journal: %s\n", strerror(-rc));
		goto out10;
	}

	/* seek to the end of the journal */
	if ((rc = sd_journal_seek_tail(journal)) < 0) {
		fprintf(stderr, "Failed to seek to the tail: %s\n", strerror(-rc));
		goto out20;
	}

	/* we are behind the last entry, so use previous one */
	if ((rc = sd_journal_previous(journal)) < 0) {
		fprintf(stderr, "Failed to iterate to previous entry: %s\n", strerror(-rc));
		goto out20;
	}

	/* reinitialize getopt() by resetting optind to 0 */
	optind = 0;

	/* get command line options - part II*/
	while ((i = getopt_long(argc, argv, optstring, options_long, NULL)) != -1) {
		switch (i) {
			case 'a':
				if (verbose > 1)
					printf("Adding logical AND to match...\n");

				if ((rc = sd_journal_add_conjunction(journal)) < 0) {
					fprintf(stderr, "Failed to add logical AND to match.\n");
					goto out30;
				}

				break;
			case 'i':
				icon = optarg;
				break;
			case 'm':
				if (verbose > 1)
					printf("Adding match '%s'...\n", optarg);

				if ((rc = sd_journal_add_match(journal, optarg, 0)) < 0) {
					fprintf(stderr, "Failed to add match '%s': %s\n", optarg, strerror(-rc));
					goto out30;
				}

				break;
			case 'o':
				if (verbose > 1)
					printf("Adding logical OR to match...\n");

				if ((rc = sd_journal_add_disjunction(journal)) < 0) {
					fprintf(stderr, "Failed to add logical OR to match.\n");
					goto out30;
				}

				break;
			case 'r':
				if (verbose > 1)
					printf("Adding regular expression '%s'...\n", optarg);

				if (have_regex > 0) {
					fprintf(stderr, "Only one regex allowed!\n");
					rc = EXIT_FAILURE;
					goto out30;
				}

				if ((rc = regcomp(&regex, optarg, regex_flags)) != 0) {
					fprintf(stderr, "Could not compile regex\n");
					goto out20;
				}
				have_regex++;

				break;
			case 't':
				timeout = atoi(optarg);
				if (verbose > 1)
					printf("Notifications will be displayed for %d seconds.\n", timeout);

				break;
		}
	}

	if (notify_init(program) == FALSE) {
		fprintf(stderr, "Failed to initialize notify.\n");
		rc = EXIT_FAILURE;
		goto out30;
	}

	while (1) {
		if ((rc = sd_journal_next(journal)) < 0) {
			fprintf(stderr, "Failed to iterate to next entry: %s\n", strerror(-rc));
			goto out40;
		} else if (rc == 0) {
			if (verbose > 2)
				printf("Waiting...\n");
			if ((rc = sd_journal_wait(journal, (uint64_t) -1)) < 0) {
				fprintf(stderr, "Failed to wait for changes: %s\n", strerror(-rc));
				goto out40;
			}
			continue;
		}

		/* get MESSAGE field */
		if ((rc = sd_journal_get_data(journal, "MESSAGE", &data, &length)) < 0) {
			fprintf(stderr, "Failed to read message field: %s\n", strerror(-rc));
			continue;
		}
		message = g_markup_escape_text(data + 8, length - 8);

		/* get SYSLOG_IDENTIFIER field */
		if ((rc = sd_journal_get_data(journal, "SYSLOG_IDENTIFIER", &data, &length)) < 0) {
			fprintf(stderr, "Failed to read syslog identifier field: %s\n", strerror(-rc));
			continue;
		}
		summary = g_markup_escape_text(data + 18, length - 18);

		if (verbose > 2)
			printf("Received message from journal: %s\n", message);

		/* show notification */
		if (have_regex == 0 || regexec(&regex, message, 0, NULL, 0) == 0) {
			for (i = 0; i < 3; i++) {
				if (verbose > 0)
					printf("Showing notification: %s: %s\n", summary, message);

				if ((rc = notify(summary, message, icon, timeout)) == 0)
					break;

				fprintf(stderr, "Failed to show notification, reinitializing libnotify.\n");
				notify_uninit();
				usleep(500 * 1000);
				if (notify_init(program) == FALSE) {
					fprintf(stderr, "Failed to initialize notify.\n");
					rc = EXIT_FAILURE;
				}
			}
			if (rc != 0)
				goto out40;
		}

		free(summary);
		free(message);
	}

	rc = EXIT_SUCCESS;

out40:
	notify_uninit();

out30:
	if (have_regex > 0)
		regfree(&regex);

out20:
	sd_journal_close(journal);

out10:
	return rc;
}

// vim: set syntax=c:
