/*
 * Copyright (c) 2010-2012 Michael Kuhn
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 **/

#include <glib.h>

#ifdef HAVE_HDTRACE
#include <hdTrace.h>
#include <hdStats.h>
#endif

#ifdef HAVE_OTF
#include <otf.h>
#endif

#include <jtrace-internal.h>

/**
 * \defgroup JTrace Trace
 *
 * The JTrace framework offers abstracted trace capabilities.
 * It can use normal terminal output, HDTrace and OTF.
 *
 * @{
 **/

/**
 * A trace.
 * Usually one trace per thread is used.
 **/
struct JTrace
{
	/**
	 * Thread name.
	 * “Main thread” or “Thread %d”.
	 **/
	gchar* thread_name;

	/**
	 * Name of the thread function.
	 **/
	gchar* function_name;

	/**
	 * Function depth within the current thread.
	 **/
	guint function_depth;

#ifdef HAVE_HDTRACE
	/**
	 * HDTrace-specific structure.
	 **/
	struct
	{
		/**
		 * Thread's topology node.
		 **/
		hdTopoNode* topo_node;

		/**
		 * Thread's trace.
		 **/
		hdTrace* trace;
	}
	hdtrace;
#endif

#ifdef HAVE_OTF
	/**
	 * OTF-specific structure.
	 **/
	struct
	{
		/**
		 * Thread's process ID.
		 **/
		guint32 process_id;
	}
	otf;
#endif

	/**
	 * The reference count.
	 **/
	gint ref_count;
};

enum JTraceFlags
{
	J_TRACE_OFF     = 0,
	J_TRACE_ECHO    = 1 << 0,
	J_TRACE_HDTRACE = 1 << 1,
	J_TRACE_OTF     = 1 << 2
};

typedef enum JTraceFlags JTraceFlags;

static JTraceFlags j_trace_flags = J_TRACE_OFF;

static gchar* j_trace_name = NULL;
static gint j_trace_thread_id = 1;

static GPatternSpec** j_trace_function_patterns = NULL;

#ifdef HAVE_HDTRACE
static hdTopology* hdtrace_topology = NULL;
static hdTopoNode* hdtrace_topo_node = NULL;

static GHashTable* hdtrace_counter_table = NULL;
#endif

#ifdef HAVE_OTF
static OTF_FileManager* otf_manager = NULL;
static OTF_Writer* otf_writer = NULL;

static guint32 otf_process_id = 0;
static guint32 otf_function_id = 0;
static guint32 otf_file_id = 0;
static guint32 otf_counter_id = 0;

static GHashTable* otf_function_table = NULL;
static GHashTable* otf_file_table = NULL;
static GHashTable* otf_counter_table = NULL;
#endif

static
void
j_trace_thread_default_free (gpointer data)
{
	JTrace* trace = data;

	j_trace_unref(trace);
}

static GPrivate j_trace_thread_default = G_PRIVATE_INIT(j_trace_thread_default_free);

G_LOCK_DEFINE_STATIC(j_trace_echo);

/**
 * Prints a common header to stderr.
 *
 * \private
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace     A trace.
 * \param timestamp A timestamp.
 **/
static
void
j_trace_echo_printerr (JTrace* trace, guint64 timestamp)
{
	guint i;

	g_printerr("[%" G_GUINT64_FORMAT ".%06" G_GUINT64_FORMAT "] %s: ", timestamp / G_USEC_PER_SEC, timestamp % G_USEC_PER_SEC, trace->thread_name);

	for (i = 0; i < trace->function_depth; i++)
	{
		g_printerr("  ");
	}
}

#ifdef HAVE_HDTRACE
/**
 * Frees the memory allocated by a HDTrace stats group.
 *
 * \private
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param data A HDTrace stats group.
 **/
static
void
j_trace_hdtrace_counter_free (gpointer data)
{
	hdStatsGroup* stats_group = data;

	hdS_disableGroup(stats_group);
	hdS_finalize(stats_group);
}
#endif

/**
 * Returns the current time.
 *
 * \private
 *
 * \author Michael Kuhn
 *
 * \code
 * guint64 timestamp;
 *
 * timestamp = j_trace_get_time();
 * \endcode
 *
 * \return A time stamp in microseconds.
 **/
static
guint64
j_trace_get_time (void)
{
	GTimeVal timeval;
	guint64 timestamp;

	g_get_current_time(&timeval);
	timestamp = (timeval.tv_sec * G_USEC_PER_SEC) + timeval.tv_usec;

	return timestamp;
}

/**
 * Returns the name of a file operation.
 *
 * \private
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param op A file operation.
 *
 * \return A name.
 **/
static
gchar const*
j_trace_file_operation_name (JTraceFileOperation op)
{
	switch (op)
	{
		case J_TRACE_FILE_CLOSE:
			return "close";
		case J_TRACE_FILE_CREATE:
			return "create";
		case J_TRACE_FILE_DELETE:
			return "delete";
		case J_TRACE_FILE_OPEN:
			return "open";
		case J_TRACE_FILE_READ:
			return "read";
		case J_TRACE_FILE_SEEK:
			return "seek";
		case J_TRACE_FILE_SYNC:
			return "sync";
		case J_TRACE_FILE_WRITE:
			return "write";
		default:
			g_warn_if_reached();
			return NULL;
	}
}

static
gboolean
j_trace_function_check (gchar const* name)
{
	if (j_trace_function_patterns != NULL)
	{
		guint i;

		for (i = 0; j_trace_function_patterns[i] != NULL; i++)
		{
			if (g_pattern_match_string(j_trace_function_patterns[i], name))
			{
				return TRUE;
			}
		}

		return FALSE;
	}

	return TRUE;
}

/**
 * Initializes the trace framework.
 * Tracing is disabled by default.
 * Set the \c J_TRACE environment variable to enable it.
 * Valid values are \e echo, \e hdtrace and \e otf.
 * Multiple values can be combined with commas.
 *
 * \author Michael Kuhn
 *
 * \code
 * j_trace_init("JULEA");
 * \endcode
 *
 * \param name A trace name.
 **/
void
j_trace_init (gchar const* name)
{
	gchar const* j_trace;
	gchar const* j_trace_function;

	g_return_if_fail(name != NULL);
	g_return_if_fail(j_trace_flags == J_TRACE_OFF);

	if ((j_trace = g_getenv("J_TRACE")) == NULL)
	{
		return;
	}
	else
	{
		gchar** p;
		guint i;
		guint l;

		p = g_strsplit(j_trace, ",", 0);
		l = g_strv_length(p);

		for (i = 0; i < l; i++)
		{
			if (g_strcmp0(p[i], "echo") == 0)
			{
				j_trace_flags |= J_TRACE_ECHO;
			}
			else if (g_strcmp0(p[i], "hdtrace") == 0)
			{
				j_trace_flags |= J_TRACE_HDTRACE;
			}
			else if (g_strcmp0(p[i], "otf") == 0)
			{
				j_trace_flags |= J_TRACE_OTF;
			}
		}

		g_strfreev(p);
	}

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	if ((j_trace_function = g_getenv("J_TRACE_FUNCTION")) != NULL)
	{
		gchar** p;
		guint i;
		guint l;

		p = g_strsplit(j_trace_function, ",", 0);
		l = g_strv_length(p);

		j_trace_function_patterns = g_new(GPatternSpec*, l + 1);

		for (i = 0; i < l; i++)
		{
			j_trace_function_patterns[i] = g_pattern_spec_new(p[i]);
		}

		j_trace_function_patterns[l] = NULL;

		g_strfreev(p);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		gchar const* levels[] = { "Host", "Process", "Thread" };
		gchar const* topo_path[] = { g_get_host_name(), name };

		hdTrace_init();

		hdtrace_topology = hdT_createTopology("JULEA", levels, 3);
		g_assert(hdtrace_topology != NULL);

		hdtrace_topo_node = hdT_createTopoNode(hdtrace_topology, topo_path, 2);
		g_assert(hdtrace_topo_node != NULL);

		hdtrace_counter_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, j_trace_hdtrace_counter_free);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		otf_process_id = 1;
		otf_function_id = 1;
		otf_file_id = 1;
		otf_counter_id = 1;

		otf_manager = OTF_FileManager_open(1);
		g_assert(otf_manager != NULL);

		otf_writer = OTF_Writer_open(name, 1, otf_manager);
		g_assert(otf_writer != NULL);

		otf_function_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		otf_file_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		otf_counter_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	}
#endif

	g_free(j_trace_name);
	j_trace_name = g_strdup(name);
}

/**
 * Shuts down the trace framework.
 *
 * \author Michael Kuhn
 *
 * \code
 * j_trace_fini();
 * \endcode
 **/
void
j_trace_fini (void)
{
	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		g_hash_table_unref(hdtrace_counter_table);
		hdtrace_counter_table = NULL;

		hdT_destroyTopoNode(hdtrace_topo_node);
		hdtrace_topo_node = NULL;

		hdT_destroyTopology(hdtrace_topology);
		hdtrace_topology = NULL;
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		g_hash_table_unref(otf_function_table);
		otf_function_table = NULL;

		g_hash_table_unref(otf_file_table);
		otf_file_table = NULL;

		g_hash_table_unref(otf_counter_table);
		otf_counter_table = NULL;

		OTF_Writer_close(otf_writer);
		otf_writer = NULL;

		OTF_FileManager_close(otf_manager);
		otf_manager = NULL;
	}
#endif

	j_trace_flags = J_TRACE_OFF;

	if (j_trace_function_patterns != NULL)
	{
		guint i;

		for (i = 0; j_trace_function_patterns[i] != NULL; i++)
		{
			g_pattern_spec_free(j_trace_function_patterns[i]);
		}
	}

	g_free(j_trace_function_patterns);

	g_free(j_trace_name);
}

JTrace*
j_trace_get_thread_default (void)
{
	return g_private_get(&j_trace_thread_default);
}

void
j_trace_set_thread_default (JTrace* trace)
{
	if (trace != NULL)
	{
		j_trace_ref(trace);
	}

	g_private_replace(&j_trace_thread_default, trace);
}

/**
 * Traces the entering of a thread.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param thread         A thread.
 *
 * \return A new trace. Should be freed with j_trace_thread_leave().
 **/
JTrace*
j_trace_new (GThread* thread)
{
	JTrace* trace;

	if (j_trace_flags == J_TRACE_OFF)
	{
		return NULL;
	}

	trace = g_slice_new(JTrace);
	trace->function_depth = 0;

	if (thread == NULL)
	{
		trace->thread_name = g_strdup("Main process");
	}
	else
	{
		trace->thread_name = g_strdup_printf("Thread %d", j_trace_thread_id);
		j_trace_thread_id++;
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		gchar const* topo_path[] = { g_get_host_name(), j_trace_name, trace->thread_name };

		trace->hdtrace.topo_node = hdT_createTopoNode(hdtrace_topology, topo_path, 3);
		trace->hdtrace.trace = hdT_createTrace(trace->hdtrace.topo_node);

		hdT_enableTrace(trace->hdtrace.trace);
		hdT_setNestedDepth(trace->hdtrace.trace, 3);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		trace->otf.process_id = otf_process_id;
		otf_process_id++;

		OTF_Writer_writeDefProcess(otf_writer, 1, trace->otf.process_id, trace->thread_name, 0);
	}
#endif

	trace->ref_count = 1;

	return trace;
}

/**
 * Increases a trace's reference count.
 *
 * \author Michael Kuhn
 *
 * \code
 * JTrace* t;
 *
 * j_trace_ref(t);
 * \endcode
 *
 * \param trace A trace.
 *
 * \return #trace.
 **/
JTrace*
j_trace_ref (JTrace* trace)
{
	if (j_trace_flags == J_TRACE_OFF)
	{
		return NULL;
	}

	g_return_val_if_fail(trace != NULL, NULL);

	g_atomic_int_inc(&(trace->ref_count));

	return trace;
}

/**
 * Decreases a trace's reference count.
 * When the reference count reaches zero, frees the memory allocated for the trace.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace A trace.
 **/
void
j_trace_unref (JTrace* trace)
{
	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	g_return_if_fail(trace != NULL);

	if (g_atomic_int_dec_and_test(&(trace->ref_count)))
	{
#ifdef HAVE_HDTRACE
		if (j_trace_flags & J_TRACE_HDTRACE)
		{
			hdT_finalize(trace->hdtrace.trace);
			hdT_destroyTopoNode(trace->hdtrace.topo_node);
		}
#endif

		g_free(trace->thread_name);

		g_slice_free(JTrace, trace);
	}
}

/**
 * Traces the entering of a function.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace A trace.
 * \param name  A function name.
 **/
void
j_trace_enter (JTrace* trace, gchar const* name)
{
	guint64 timestamp;

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	g_return_if_fail(trace != NULL);
	g_return_if_fail(name != NULL);

	if (!j_trace_function_check(name))
	{
		/* FIXME also blacklist nested functions */
		return;
	}

	timestamp = j_trace_get_time();

	if (j_trace_flags & J_TRACE_ECHO)
	{
		G_LOCK(j_trace_echo);
		j_trace_echo_printerr(trace, timestamp);
		g_printerr("ENTER %s\n", name);
		G_UNLOCK(j_trace_echo);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		hdT_logStateStart(trace->hdtrace.trace, name);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		gpointer value;
		guint32 function_id;

		if ((value = g_hash_table_lookup(otf_function_table, name)) == NULL)
		{
			function_id = otf_function_id;
			otf_function_id++;

			OTF_Writer_writeDefFunction(otf_writer, 1, function_id, name, 0, 0);
			g_hash_table_insert(otf_function_table, g_strdup(name), GUINT_TO_POINTER(function_id));
		}
		else
		{
			function_id = GPOINTER_TO_UINT(value);
		}

		OTF_Writer_writeEnter(otf_writer, timestamp, function_id, trace->otf.process_id, 0);
	}
#endif

	trace->function_depth++;
}

/**
 * Traces the leaving of a function.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace A trace.
 * \param name  A function name.
 **/
void
j_trace_leave (JTrace* trace, gchar const* name)
{
	guint64 timestamp;

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	g_return_if_fail(trace != NULL);
	g_return_if_fail(name != NULL);

	if (!j_trace_function_check(name))
	{
		return;
	}

	/* FIXME */
	if (trace->function_depth == 0)
	{
		return;
	}

	trace->function_depth--;
	timestamp = j_trace_get_time();

	if (j_trace_flags & J_TRACE_ECHO)
	{
		G_LOCK(j_trace_echo);
		j_trace_echo_printerr(trace, timestamp);
		g_printerr("LEAVE %s\n", name);
		G_UNLOCK(j_trace_echo);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		hdT_logStateEnd(trace->hdtrace.trace);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		gpointer value;
		guint32 function_id;

		value = g_hash_table_lookup(otf_function_table, name);
		g_assert(value != NULL);
		function_id = GPOINTER_TO_UINT(value);

		OTF_Writer_writeLeave(otf_writer, timestamp, function_id, trace->otf.process_id, 0);
	}
#endif
}

/**
 * Traces the beginning of a file operation.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace A trace.
 * \param path  A file path.
 * \param op    A file operation.
 **/
void
j_trace_file_begin (JTrace* trace, gchar const* path, JTraceFileOperation op)
{
	guint64 timestamp;

	g_return_if_fail(path != NULL);

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	timestamp = j_trace_get_time();

	if (j_trace_flags & J_TRACE_ECHO)
	{
		G_LOCK(j_trace_echo);
		j_trace_echo_printerr(trace, timestamp);
		g_printerr("BEGIN %s %s\n", j_trace_file_operation_name(op), path);
		G_UNLOCK(j_trace_echo);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		hdT_logStateStart(trace->hdtrace.trace, j_trace_file_operation_name(op));
		hdT_logAttributes(trace->hdtrace.trace, "path='%s'", path);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		gpointer value;
		guint32 file_id;

		if ((value = g_hash_table_lookup(otf_file_table, path)) == NULL)
		{
			file_id = otf_file_id;
			otf_file_id++;

			OTF_Writer_writeDefFile(otf_writer, 1, file_id, path, 0);
			g_hash_table_insert(otf_file_table, g_strdup(path), GUINT_TO_POINTER(file_id));
		}
		else
		{
			file_id = GPOINTER_TO_UINT(value);
		}

		OTF_Writer_writeBeginFileOperation(otf_writer, timestamp, trace->otf.process_id, 1, 0);
	}
#endif

	return;
}

/**
 * Traces the ending of a file operation.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace  A trace.
 * \param path   A file path.
 * \param op     A file operation.
 * \param length A length.
 * \param offset An offset.
 **/
void
j_trace_file_end (JTrace* trace, gchar const* path, JTraceFileOperation op, guint64 length, guint64 offset)
{
	guint64 timestamp;

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	timestamp = j_trace_get_time();

	if (j_trace_flags & J_TRACE_ECHO)
	{
		G_LOCK(j_trace_echo);
		j_trace_echo_printerr(trace, timestamp);
		g_printerr("END %s %s", j_trace_file_operation_name(op), path);

		if (op == J_TRACE_FILE_READ || op == J_TRACE_FILE_WRITE)
		{
			g_printerr(" (length=%" G_GUINT64_FORMAT ", offset=%" G_GUINT64_FORMAT ")", length, offset);
		}

		g_printerr("\n");
		G_UNLOCK(j_trace_echo);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		if (op == J_TRACE_FILE_READ || op == J_TRACE_FILE_WRITE)
		{
			hdT_logAttributes(trace->hdtrace.trace, "length='%" G_GUINT64_FORMAT "'", length);
			hdT_logAttributes(trace->hdtrace.trace, "offset='%" G_GUINT64_FORMAT "'", offset);
		}

		hdT_logStateEnd(trace->hdtrace.trace);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		gpointer value;
		guint32 file_id;
		guint32 otf_op;

		switch (op)
		{
			case J_TRACE_FILE_CLOSE:
				otf_op = OTF_FILEOP_CLOSE;
				break;
			case J_TRACE_FILE_CREATE:
				otf_op = OTF_FILEOP_OTHER;
				break;
			case J_TRACE_FILE_DELETE:
				otf_op = OTF_FILEOP_UNLINK;
				break;
			case J_TRACE_FILE_OPEN:
				otf_op = OTF_FILEOP_OPEN;
				break;
			case J_TRACE_FILE_READ:
				otf_op = OTF_FILEOP_READ;
				break;
			case J_TRACE_FILE_SEEK:
				otf_op = OTF_FILEOP_SEEK;
				break;
			case J_TRACE_FILE_SYNC:
				otf_op = OTF_FILEOP_SYNC;
				break;
			case J_TRACE_FILE_WRITE:
				otf_op = OTF_FILEOP_WRITE;
				break;
			default:
				g_return_if_reached();
		}

		value = g_hash_table_lookup(otf_file_table, path);
		g_assert(value != NULL);
		file_id = GPOINTER_TO_UINT(value);

		OTF_Writer_writeEndFileOperation(otf_writer, timestamp, trace->otf.process_id, file_id, 1, 0, otf_op, length, 0);
	}
#endif
}

/**
 * Traces a counter.
 *
 * \author Michael Kuhn
 *
 * \code
 * \endcode
 *
 * \param trace         A trace.
 * \param name          A counter name.
 * \param counter_value A counter value.
 **/
void
j_trace_counter (JTrace* trace, gchar const* name, guint64 counter_value)
{
	guint64 timestamp;

	if (j_trace_flags == J_TRACE_OFF)
	{
		return;
	}

	g_return_if_fail(trace != NULL);
	g_return_if_fail(name != NULL);

	timestamp = j_trace_get_time();

	if (j_trace_flags & J_TRACE_ECHO)
	{
		G_LOCK(j_trace_echo);
		j_trace_echo_printerr(trace, timestamp);
		g_printerr("COUNTER %s %" G_GUINT64_FORMAT "\n", name, counter_value);
		G_UNLOCK(j_trace_echo);
	}

#ifdef HAVE_HDTRACE
	if (j_trace_flags & J_TRACE_HDTRACE)
	{
		hdStatsGroup* stats_group;

		if ((stats_group = g_hash_table_lookup(hdtrace_counter_table, name)) == NULL)
		{
			stats_group = hdS_createGroup(name, hdtrace_topo_node, 2);
			hdS_addValue(stats_group, name, UINT64, "B", "julead");
			hdS_commitGroup(stats_group);
			hdS_enableGroup(stats_group);

			g_hash_table_insert(hdtrace_counter_table, g_strdup(name), stats_group);
		}

		hdS_writeUInt64Value(stats_group, counter_value);
	}
#endif

#ifdef HAVE_OTF
	if (j_trace_flags & J_TRACE_OTF)
	{
		gpointer value;
		guint32 counter_id;

		if ((value = g_hash_table_lookup(otf_counter_table, name)) == NULL)
		{
			counter_id = otf_counter_id;
			otf_counter_id++;

			OTF_Writer_writeDefCounter(otf_writer, 1, counter_id, name, OTF_COUNTER_TYPE_ABS + OTF_COUNTER_SCOPE_LAST, 0, NULL);
			g_hash_table_insert(otf_counter_table, g_strdup(name), GUINT_TO_POINTER(counter_id));
		}
		else
		{
			counter_id = GPOINTER_TO_UINT(value);
		}

		OTF_Writer_writeCounter(otf_writer, timestamp, trace->otf.process_id, counter_id, counter_value);
	}
#endif
}

/**
 * @}
 **/
