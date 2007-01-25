#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "local_master.h"

/********************************************************************
 * Global variables
 */

DC_ResultCallback	_dc_resultcb;
DC_SubresultCallback	_dc_subresultcb;
DC_MessageCallback	_dc_messagecb;

char project_uuid_str[37];
uuid_t project_uuid;
int sleep_interval;

static GHashTable *wu_table;

/********************************************************************
 * API functions
 */

int DC_initMaster(const char *config_file)
{
	char *cfgval;
	int ret;

	if (!config_file)
		config_file = DC_CONFIG_FILE;

	ret = _DC_parseCfg(config_file);
	if (ret)
	{
		DC_log(LOG_ERR, "The DC-API cannot be initialized without a "
			"config file");
		return ret;
	}

	/* Check the working directory */
	cfgval = DC_getCfgStr(CFG_WORKDIR);
	if (!cfgval)
	{
		DC_log(LOG_ERR, "%s is not specified in the config file",
			CFG_WORKDIR);
		return DC_ERR_CONFIG;
	}
	free(cfgval);

	/* Check sleep interval */
	sleep_interval = DC_getCfgInt(CFG_SLEEPINTERVAL, DEFAULT_SLEEP_INTERVAL);
	if (sleep_interval < 1)
		sleep_interval = 1;

	/* Check the project UUID */
	cfgval = DC_getCfgStr(CFG_INSTANCEUUID);
	if (!cfgval)
	{
		DC_log(LOG_ERR, "%s is not set in the config file",
			CFG_INSTANCEUUID);
		return DC_ERR_CONFIG;
	}

	ret = uuid_parse((char *)cfgval, project_uuid);
	if (ret)
	{
		DC_log(LOG_ERR, "Invalid project UUID");
		free(cfgval);
		return DC_ERR_CONFIG;
	}
	free(cfgval);

	/* Enforce a canonical string representation of the UUID */
	uuid_unparse_lower(project_uuid, project_uuid_str);

	return DC_OK;
}

int DC_getMaxMessageSize(void)
{
	return MAX_MESSAGE_SIZE;
}

int DC_getMaxSubresults(void)
{
	return MAX_SUBRESULTS;
}

unsigned DC_getGridCapabilities(void)
{
	return DC_GC_STDOUT | DC_GC_STDERR;
}

void DC_setMasterCb(DC_ResultCallback resultcb, DC_SubresultCallback subresultcb,
	DC_MessageCallback msgcb)
{
	_dc_resultcb = resultcb;
	_dc_subresultcb = subresultcb;
	_dc_messagecb = msgcb;
}


/********************************************************************
 * Functions
 */

static char *get_workdir(const uuid_t uuid, int create)
{
	char *tmp, uuid_str[37], *cfgval;
	GString *str;
	int ret;

	uuid_unparse_lower(uuid, uuid_str);

	cfgval = DC_getCfgStr(CFG_WORKDIR);
	if (!cfgval)
		return NULL;
	str = g_string_new(cfgval);
	free(cfgval);

	if (create)
	{
		ret = mkdir(str->str, 0755);
		if (ret && errno != EEXIST)
			goto error;
	}

	g_string_append_c(str, G_DIR_SEPARATOR);
	g_string_append(str, ".dcapi-");
	g_string_append(str, project_uuid_str);
	if (create)
	{
		ret = mkdir(str->str, 0755);
		if (ret && errno != EEXIST)
			goto error;
	}

	g_string_append_c(str, G_DIR_SEPARATOR);
	g_string_append_printf(str, "%02x", uuid[0]);
	if (create)
	{
		ret = mkdir(str->str, 0755);
		if (ret && errno != EEXIST)
			goto error;
	}

	g_string_append_c(str, G_DIR_SEPARATOR);
	g_string_append(str, uuid_str);
	if (create)
	{
		ret = mkdir(str->str, 0755);
		if (ret && errno != EEXIST)
			goto error;
	}

	tmp = str->str;
	g_string_free(str, FALSE);
	return tmp;

error:
	DC_log(LOG_ERR, "Failed to create WU working directory %s: %s",
		str->str, strerror(errno));
	g_string_free(str, TRUE);
	return NULL;
}


DC_Workunit *DC_createWU(const char *clientName, const char *arguments[],
	int subresults, const char *tag)
{
	char uuid_str[37];
	DC_Workunit *wu;

	if (subresults > MAX_SUBRESULTS){
		DC_log(LOG_ERR, "DC_createWU: The given subresult number: %d is too high. (max:%d)",
			subresults, MAX_SUBRESULTS);
		return NULL;
	}

	if (!clientName)
	{
		DC_log(LOG_ERR, "DC_createWU: clientName is not supplied");
		return NULL;
	}

	wu = g_new0(DC_Workunit, 1);

	wu->argv = g_strdupv((char **)arguments);

	for (wu->argc = 0; arguments && arguments[wu->argc]; wu->argc++)
		/* Nothing */;

	wu->subresults = subresults;
	wu->tag = g_strdup(tag);

	uuid_generate(wu->uuid);
	uuid_unparse_lower(wu->uuid, uuid_str);
	wu->uuid_str = g_strdup(uuid_str);

	if (tag)
		wu->name = g_strdup_printf("%s_%s_%s", project_uuid_str,
			uuid_str, tag);
	else
		wu->name = g_strdup_printf("%s_%s", project_uuid_str, uuid_str);

	/* Calculate & create the working directory. The working directory
	 * has the form:
	 * <project work dir>/.dcapi-<project uuid>/<hash>/<wu uuid>
	 * Where <hash> is the first 2 hex digits of the uuid
	 */
	wu->workdir = get_workdir(wu->uuid, TRUE);
	if (!wu->workdir)
	{
		DC_destroyWU(wu);
		return NULL;
	}

	wu->client_name = DC_getClientCfgStr(clientName, "name", FALSE);
	wu->client_path = DC_getClientCfgStr(clientName, "path", FALSE);

	if (!wu->client_name || !wu->client_path)
	{
		DC_log(LOG_ERR, "Failed to create WU. Cannot find client name\n"
			"Define client application in the config file:\n"
			"[Client-%s]\nname = <client name>\npath = <client path>", clientName);
		DC_destroyWU(wu);
		return NULL;
	}

	DC_log(LOG_DEBUG, "client path: %s,     client name: %s    from client: %s",
		wu->client_path, wu->client_name, clientName);

	if (!wu_table)
		wu_table = g_hash_table_new_full(g_str_hash, g_str_equal,
			NULL, NULL);
	g_hash_table_insert(wu_table, wu->name, wu);

	return wu;
}


static char *get_workdir_path(DC_Workunit *wu, const char *label,
	WorkdirFile type)
{
	return g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, label);
}

void DC_destroyWU(DC_Workunit *wu)
{
	char *path;

	if (!wu)
		return;

	if (wu_table)
		g_hash_table_remove(wu_table, wu->name);

	switch (wu->state)
	{
		case DC_WU_RUNNING:
			/* XXX Abort the work unit */
			break;
		default:
			break;
	}

	while (wu->input_files)
	{
		DC_PhysicalFile *file = (DC_PhysicalFile *)wu->input_files->data;

		unlink(file->path);
		wu->input_files = g_list_delete_link(wu->input_files,
			wu->input_files);
		_DC_destroyPhysicalFile(file);
	}

	while (wu->output_files)
	{
		char *name = (char *)wu->output_files->data;
		char *file = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, name);
		unlink(file);
		g_free(file);

		g_free(wu->output_files->data);
		wu->output_files = g_list_delete_link(wu->output_files,
			wu->output_files);
	}

	/* checkpoint file */
	path = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, CKPT_LABEL);
	unlink(path);
	g_free(path);

	/* standard output file */
	path = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, STDOUT_LABEL);
	unlink(path);
	g_free(path);

	/* standard error file */
	path = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, STDERR_LABEL);
	unlink(path);
	g_free(path);

	if (wu->client_name)
	{
		char *path = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, wu->client_name);
		unlink(path);
		g_free(path);
		g_free(wu->client_name);
		g_free(wu->client_path);
	}

	if (wu->workdir)
	{
		const char *name;
		GDir *dir;
		int ret;

		dir = g_dir_open(wu->workdir, 0, NULL);
		/* The work directory should not contain any extra files, but
		 * just in case */
		while (dir && (name = g_dir_read_name(dir)))
		{
			GString *str = g_string_new(wu->workdir);
			g_string_append_c(str, G_DIR_SEPARATOR);
			g_string_append(str, name);
			DC_log(LOG_INFO, "Removing unknown file %s",
				str->str);
			unlink(str->str);
			g_string_free(str, TRUE);
		}
		if (dir)
			g_dir_close(dir);

		ret = rmdir(wu->workdir);
		if (ret)
			DC_log(LOG_WARNING, "Failed to remove WU working "
				"directory %s: %s", wu->workdir,
				strerror(errno));
		g_free(wu->workdir);
	}

	g_free(wu->uuid_str);
	g_strfreev(wu->argv);
	g_free(wu->tag);
	g_free(wu->name);

	g_free(wu);
}

/* Check if the logical name is not already registered */
static int check_logical_name(DC_Workunit *wu, const char *logicalFileName)
{
	GList *l;

	if (strchr(logicalFileName, '/') || strchr(logicalFileName, '\\'))
	{
		DC_log(LOG_ERR, "Illegal characters in logical file name %s",
			logicalFileName);
		return DC_ERR_BADPARAM;
	}
	for (l = wu->input_files; l; l = l->next)
	{
		DC_PhysicalFile *file = (DC_PhysicalFile *)l->data;

		if (!strcmp(file->label, logicalFileName))
		{
			DC_log(LOG_ERR, "File %s is already registered as an "
				"input file", logicalFileName);
			return DC_ERR_BADPARAM;
		}
	}
	for (l = wu->output_files; l; l = l->next)
	{
		if (!strcmp((char *)l->data, logicalFileName))
		{
			DC_log(LOG_ERR, "File %s is already registered as an "
				"output file", logicalFileName);
			return DC_ERR_BADPARAM;
		}
	}
	return 0;
}

#define COPY_BUFSIZE 65536
static int copy_file(const char *src, const char *dst)
{
	int sfd, dfd;
	ssize_t ret;
	char *buf;

	buf = (char *)g_malloc(COPY_BUFSIZE);
	sfd = open(src, O_RDONLY);
	if (sfd == -1)
	{
		DC_log(LOG_ERR, "Failed to open %s for copying: %s", src,
			strerror(errno));
		g_free(buf);
		return -1;
	}
	dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
	if (dfd == -1)
	{
		DC_log(LOG_ERR, "Failed to create %s: %s", dst, strerror(errno));
		g_free(buf);
		close(sfd);
		return -1;
	}

	while ((ret = read(sfd, buf, COPY_BUFSIZE)) > 0)
	{
		char *ptr = buf;
		while (ret)
		{
			ssize_t ret2 = write(dfd, ptr, ret);
			if (ret2 < 0)
			{
				DC_log(LOG_ERR, "Error writing to %s: %s", dst,
					strerror(errno));
				close(sfd);
				close(dfd);
				unlink(dst);
				g_free(buf);
				return -1;
			}
			ret -= ret2;
			ptr += ret2;
		}
	}

	if (ret < 0)
	{
		DC_log(LOG_ERR, "Error reading from %s: %s", src, strerror(errno));
		close(sfd);
		close(dfd);
		g_free(buf);
		unlink(dst);
		return -1;
	}

	g_free(buf);
	close(sfd);
	if (close(dfd))
	{
		DC_log(LOG_ERR, "Error writing to %s: %s", dst, strerror(errno));
		unlink(dst);
		return -1;
	}
	return 0;
}

int DC_addWUInput(DC_Workunit *wu, const char *logicalFileName, const char *URL,
	DC_FileMode fileMode)
{
	DC_PhysicalFile *file;
	char *workpath;
	int ret;

	/* Sanity checks */
	ret = check_logical_name(wu, logicalFileName);
	if (ret)
		return ret;

	workpath = get_workdir_path(wu, logicalFileName, FILE_IN);
	file = _DC_createPhysicalFile(logicalFileName, workpath);
	g_free(workpath);
	if (!file)
		return DC_ERR_INTERNAL;

	switch (fileMode)
	{
		case DC_FILE_REGULAR:
			ret = copy_file(URL, file->path);
			if (ret)
			{
				_DC_destroyPhysicalFile(file);
				return DC_ERR_BADPARAM; /* XXX */
			}
			break;
		case DC_FILE_PERSISTENT:
			ret = link(URL, file->path);
			if (ret)
			{
				DC_log(LOG_ERR, "Failed to link %s to %s: %s",
					URL, file->path, strerror(errno));
				_DC_destroyPhysicalFile(file);
				return DC_ERR_BADPARAM; /* XXX */
			}
			/* Remember the file mode */
			file->mode = DC_FILE_PERSISTENT;
			break;
		case DC_FILE_VOLATILE:
			ret = rename(URL, file->path);
			if (ret)
			{
				DC_log(LOG_ERR, "Failed to rename %s to %s: %s",
					URL, file->path, strerror(errno));
				_DC_destroyPhysicalFile(file);
				return DC_ERR_BADPARAM; /* XXX */
			}
			break;
	}

	wu->input_files = g_list_append(wu->input_files, file);
	wu->num_inputs++;
	return 0;
}

int DC_addWUOutput(DC_Workunit *wu, const char *logicalFileName)
{
	int ret;

	/* Sanity checks */
	ret = check_logical_name(wu, logicalFileName);
	if (ret)
		return ret;

	wu->output_files = g_list_append(wu->output_files,
		g_strdup(logicalFileName));
	wu->num_outputs++;
	return 0;
}

int DC_submitWU(DC_Workunit *wu)
{
	pid_t pid;
	char *old_path, *new_path;

	if (wu->state != DC_WU_READY)
	{
		DC_log(LOG_ERR, "Only WUs in READY state can be submitted");
		return DC_ERR_BADPARAM;
	}

	/* copy the exec into the workdir */
	old_path = g_strdup_printf("%s%c%s", wu->client_path, G_DIR_SEPARATOR, wu->client_name);
	new_path = g_strdup_printf("%s%c%s", wu->workdir, G_DIR_SEPARATOR, wu->client_name);
	if (link(old_path, new_path))
	{
		DC_log(LOG_ERR, "Failed to link %s to %s: %s",
			old_path, new_path, strerror(errno));
		return DC_ERR_BADPARAM;
	}
	g_free(old_path);
	g_free(new_path);

	if((pid=fork())<0) {
		DC_log(LOG_ERR,"Cannot fork!\nerrno=%d  %s\n",
			errno, strerror(errno));
		return DC_ERR_BADPARAM;
	}

	if(pid==0) /* client process */
	{
		/* change into working directory of the WU */
		if (chdir(wu->workdir))
		{
			DC_log(LOG_ERR,"Cannot cd into %s\nerrno=%d  %s\n",
			wu->workdir, errno, strerror(errno));
			return DC_ERR_BADPARAM;
		}

		/* hook up stdout and stderr to specially-named files */
		freopen(STDOUT_LABEL, "a", stdout);
		freopen(STDERR_LABEL, "a", stderr);

		/* execute the client */
		DC_log(LOG_INFO, "Work unit : %s executes: %s", wu->name, wu->client_name);
		execv(wu->client_name, wu->argv);
		DC_log(LOG_ERR, "Cannot execute. Errno=%d  %s\n",
			errno, strerror(errno));
		exit(1);
	}

	wu->pid = pid;
	wu->state = DC_WU_RUNNING;

	return DC_OK;
}

DC_Workunit *_DC_getWUByName(const char *name)
{
	DC_Workunit *wu;
	char *uuid_str;
	uuid_t uuid;
	int ret;

	if (wu_table)
	{
		wu = (DC_Workunit *)g_hash_table_lookup(wu_table, name);
		if (wu)
			return wu;
	}

	/* Check if the WU belongs to this application */
	uuid_str = g_strndup(name, 36);
	ret = uuid_parse(uuid_str, uuid);
	g_free(uuid_str);
	if (ret)
	{
		DC_log(LOG_ERR, "WU name contains illegal UUID");
		return NULL;
	}

	if (uuid_compare(uuid, project_uuid))
	{
		DC_log(LOG_WARNING, "WU does not belong to this application");
		return NULL;
	}

	if (name[36] != '_')
	{
		DC_log(LOG_ERR, "Illegal WU name syntax");
		return NULL;
	}

	/* Check the WU's UUID */
	uuid_str = g_strndup(name + 37, 36);
	ret = uuid_parse(uuid_str, uuid);
	g_free(uuid_str);
	if (ret)
	{
		DC_log(LOG_ERR, "WU name contains illegal UUID");
		return NULL;
	}

	DC_log(LOG_ERR, "WU %s not found!", name);
	return NULL;
}

static void testWUEvents(void *key, void *value, void *ptr)
{
	DC_Workunit *wu = (DC_Workunit *)value;
	DC_Result *result;
	char syscmd[256];
	int retval;

	snprintf(syscmd, 256, "ps -p %d >/dev/null", wu->pid);
	retval = system(syscmd);

	/* retval == 0 means that the process is still in output of ps
		but it can be <defunct>.
		So check it again.
	*/
	if (retval == 0) {
		snprintf(syscmd, 256, "ps -p %d | grep defunct >/dev/null",
			wu->pid);
		retval = system(syscmd);
		if (retval == 0) retval = 1; /* defunct means finished */
		else if (retval == 1) retval = 0;
	}
	if (retval == 1) { /* process finished (not exists) */
		/* create the result object */
		DC_log(LOG_INFO, "Work unit %s with pid %d is found to be finished",
			wu->name, wu->pid);
		wu->state = DC_WU_FINISHED;

		result = _DC_createResult(wu->name);
		if (result)
		{
			_dc_resultcb(result->wu, result);
			_DC_destroyResult(result);
		}
	}

	return;
}

int _DC_searchForEvents()
{
	if (!wu_table)
	{
		DC_log(LOG_WARNING, "Searching for events is only usefull if there is any running work unit!");
		return DC_ERR_BADPARAM;
	}

	g_hash_table_foreach(wu_table, (GHFunc)testWUEvents, NULL);

	return DC_OK;
}

static DC_WUState matchState;
static void countState(void *key, void *value, void *ptr)
{
	DC_Workunit *wu = (DC_Workunit *)value;
	int *count = (int *)ptr;

	if (wu->state == matchState) ++(*count);
}

int DC_getWUNumber(DC_WUState state)
{
	int val = 0;

	matchState = state;
	g_hash_table_foreach(wu_table, (GHFunc)countState, &val);

	return val;
}

char *DC_getWUId(const DC_Workunit *wu)
{
	char *id, *ret;

	id = g_strdup_printf("%d", wu->pid);
	ret = strdup(id);

	g_free(id);
	return ret;
}

char *DC_getWUTag(const DC_Workunit *wu)
{
	char *tag;

	tag = strdup(wu->tag);

	return tag;
}

int DC_setWUPriority(DC_Workunit *wu, int priority)
{
	return 0;
}
