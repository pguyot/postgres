/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/hooks.c
 *
 * Entrypoints of the hooks in PostgreSQL, and dispatches the callbacks.
 *
 * Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/seclabel.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/guc.h"

#include "sepgsql.h"

PG_MODULE_MAGIC;

/*
 * Declarations
 */
void		_PG_init(void);

/*
 * Saved hook entries (if stacked)
 */
static object_access_hook_type next_object_access_hook = NULL;
static ClientAuthentication_hook_type next_client_auth_hook = NULL;
static ExecutorCheckPerms_hook_type next_exec_check_perms_hook = NULL;
static needs_fmgr_hook_type next_needs_fmgr_hook = NULL;
static fmgr_hook_type next_fmgr_hook = NULL;
static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;
static ExecutorStart_hook_type next_ExecutorStart_hook = NULL;

/*
 * Contextual information on DDL commands
 */
typedef struct
{
	NodeTag		cmdtype;

	/*
	 * Name of the template database given by users on CREATE DATABASE
	 * command. Elsewhere (including the case of default) NULL.
	 */
	const char *createdb_dtemplate;
} sepgsql_context_info_t;

static sepgsql_context_info_t	sepgsql_context_info;

/*
 * GUC: sepgsql.permissive = (on|off)
 */
static bool sepgsql_permissive;

bool
sepgsql_get_permissive(void)
{
	return sepgsql_permissive;
}

/*
 * GUC: sepgsql.debug_audit = (on|off)
 */
static bool sepgsql_debug_audit;

bool
sepgsql_get_debug_audit(void)
{
	return sepgsql_debug_audit;
}

/*
 * sepgsql_client_auth
 *
 * Entrypoint of the client authentication hook.
 * It switches the client label according to getpeercon(), and the current
 * performing mode according to the GUC setting.
 */
static void
sepgsql_client_auth(Port *port, int status)
{
	char	   *context;

	if (next_client_auth_hook)
		(*next_client_auth_hook) (port, status);

	/*
	 * In the case when authentication failed, the supplied socket shall be
	 * closed soon, so we don't need to do anything here.
	 */
	if (status != STATUS_OK)
		return;

	/*
	 * Getting security label of the peer process using API of libselinux.
	 */
	if (getpeercon_raw(port->sock, &context) < 0)
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: unable to get peer label: %m")));

	sepgsql_set_client_label(context);

	/*
	 * Switch the current performing mode from INTERNAL to either DEFAULT or
	 * PERMISSIVE.
	 */
	if (sepgsql_permissive)
		sepgsql_set_mode(SEPGSQL_MODE_PERMISSIVE);
	else
		sepgsql_set_mode(SEPGSQL_MODE_DEFAULT);
}

/*
 * sepgsql_object_access
 *
 * Entrypoint of the object_access_hook. This routine performs as
 * a dispatcher of invocation based on access type and object classes.
 */
static void
sepgsql_object_access(ObjectAccessType access,
					  Oid classId,
					  Oid objectId,
					  int subId)
{
	if (next_object_access_hook)
		(*next_object_access_hook) (access, classId, objectId, subId);

	switch (access)
	{
		case OAT_POST_CREATE:
			switch (classId)
			{
				case DatabaseRelationId:
					sepgsql_database_post_create(objectId,
								sepgsql_context_info.createdb_dtemplate);
					break;

				case NamespaceRelationId:
					sepgsql_schema_post_create(objectId);
					break;

				case RelationRelationId:
					if (subId == 0)
					{
						/*
						 * All cases we want to apply permission checks on
						 * creation of a new relation are invocation of the
						 * heap_create_with_catalog via DefineRelation or
						 * OpenIntoRel.
						 * Elsewhere, we need neither assignment of security
						 * label nor permission checks.
						 */
						switch (sepgsql_context_info.cmdtype)
						{
							case T_CreateStmt:
							case T_ViewStmt:
							case T_CreateSeqStmt:
							case T_CompositeTypeStmt:
							case T_CreateForeignTableStmt:
							case T_SelectStmt:
								sepgsql_relation_post_create(objectId);
								break;
							default:
								/* via make_new_heap() */
								break;
						}
					}
					else
						sepgsql_attribute_post_create(objectId, subId);
					break;

				case ProcedureRelationId:
					sepgsql_proc_post_create(objectId);
					break;

				default:
					/* Ignore unsupported object classes */
					break;
			}
			break;

		default:
			elog(ERROR, "unexpected object access type: %d", (int) access);
			break;
	}
}

/*
 * sepgsql_exec_check_perms
 *
 * Entrypoint of DML permissions
 */
static bool
sepgsql_exec_check_perms(List *rangeTabls, bool abort)
{
	/*
	 * If security provider is stacking and one of them replied 'false' at
	 * least, we don't need to check any more.
	 */
	if (next_exec_check_perms_hook &&
		!(*next_exec_check_perms_hook) (rangeTabls, abort))
		return false;

	if (!sepgsql_dml_privileges(rangeTabls, abort))
		return false;

	return true;
}

/*
 * sepgsql_needs_fmgr_hook
 *
 * It informs the core whether the supplied function is trusted procedure,
 * or not. If true, sepgsql_fmgr_hook shall be invoked at start, end, and
 * abort time of function invocation.
 */
static bool
sepgsql_needs_fmgr_hook(Oid functionId)
{
	ObjectAddress	object;

	if (next_needs_fmgr_hook &&
		(*next_needs_fmgr_hook) (functionId))
		return true;

	/*
	 * SELinux needs the function to be called via security_definer wrapper,
	 * if this invocation will take a domain-transition. We call these
	 * functions as trusted-procedure, if the security policy has a rule that
	 * switches security label of the client on execution.
	 */
	if (sepgsql_avc_trusted_proc(functionId) != NULL)
		return true;

	/*
	 * Even if not a trusted-procedure, this function should not be inlined
	 * unless the client has db_procedure:{execute} permission. Please note
	 * that it shall be actually failed later because of same reason with
	 * ACL_EXECUTE.
	 */
	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	if (!sepgsql_avc_check_perms(&object,
								 SEPG_CLASS_DB_PROCEDURE,
								 SEPG_DB_PROCEDURE__EXECUTE,
								 SEPGSQL_AVC_NOAUDIT, false))
		return true;

	return false;
}

/*
 * sepgsql_fmgr_hook
 *
 * It switches security label of the client on execution of trusted
 * procedures.
 */
static void
sepgsql_fmgr_hook(FmgrHookEventType event,
				  FmgrInfo *flinfo, Datum *private)
{
	struct
	{
		char	   *old_label;
		char	   *new_label;
		Datum		next_private;
	}		   *stack;

	switch (event)
	{
		case FHET_START:
			stack = (void *) DatumGetPointer(*private);
			if (!stack)
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(flinfo->fn_mcxt);
				stack = palloc(sizeof(*stack));
				stack->old_label = NULL;
				stack->new_label = sepgsql_avc_trusted_proc(flinfo->fn_oid);
				stack->next_private = 0;

				MemoryContextSwitchTo(oldcxt);

				/*
				 * process:transition permission between old and new label,
				 * when user tries to switch security label of the client
				 * on execution of trusted procedure.
				 */
				if (stack->new_label)
					sepgsql_avc_check_perms_label(stack->new_label,
												  SEPG_CLASS_PROCESS,
												  SEPG_PROCESS__TRANSITION,
												  NULL, true);

				*private = PointerGetDatum(stack);
			}
			Assert(!stack->old_label);
			if (stack->new_label)
				stack->old_label = sepgsql_set_client_label(stack->new_label);

			if (next_fmgr_hook)
				(*next_fmgr_hook) (event, flinfo, &stack->next_private);
			break;

		case FHET_END:
		case FHET_ABORT:
			stack = (void *) DatumGetPointer(*private);

			if (next_fmgr_hook)
				(*next_fmgr_hook) (event, flinfo, &stack->next_private);

			if (stack->old_label)
				sepgsql_set_client_label(stack->old_label);
			stack->old_label = NULL;
			break;

		default:
			elog(ERROR, "unexpected event type: %d", (int) event);
			break;
	}
}

/*
 * sepgsql_executor_start
 *
 * It saves contextual information during ExecutorStart to distinguish 
 * a case with/without permission checks later.
 */
static void
sepgsql_executor_start(QueryDesc *queryDesc, int eflags)
{
	sepgsql_context_info_t	saved_context_info = sepgsql_context_info;

	PG_TRY();
	{
		if (queryDesc->operation == CMD_SELECT)
			sepgsql_context_info.cmdtype = T_SelectStmt;
		else if (queryDesc->operation == CMD_INSERT)
			sepgsql_context_info.cmdtype = T_InsertStmt;
		else if (queryDesc->operation == CMD_DELETE)
			sepgsql_context_info.cmdtype = T_DeleteStmt;
		else if (queryDesc->operation == CMD_UPDATE)
			sepgsql_context_info.cmdtype = T_UpdateStmt;

		/*
		 * XXX - If queryDesc->operation is not above four cases, an error
		 * shall be raised on the following executor stage soon.
		 */
		if (next_ExecutorStart_hook)
			(*next_ExecutorStart_hook) (queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		sepgsql_context_info = saved_context_info;
		PG_RE_THROW();
	}
	PG_END_TRY();
	sepgsql_context_info = saved_context_info;
}

/*
 * sepgsql_utility_command
 *
 * It tries to rough-grained control on utility commands; some of them can
 * break whole of the things if nefarious user would use.
 */
static void
sepgsql_utility_command(Node *parsetree,
						const char *queryString,
						ParamListInfo params,
						bool isTopLevel,
						DestReceiver *dest,
						char *completionTag)
{
	sepgsql_context_info_t	saved_context_info = sepgsql_context_info;
	ListCell	   *cell;

	PG_TRY();
	{
		/*
		 * Check command tag to avoid nefarious operations, and save the
		 * current contextual information to determine whether we should
		 * apply permission checks here, or not.
		 */
		sepgsql_context_info.cmdtype = nodeTag(parsetree);

		switch (nodeTag(parsetree))
		{
			case T_CreatedbStmt:
				/*
				 * We hope to reference name of the source database, but it
				 * does not appear in system catalog. So, we save it here.
				 */
				foreach (cell, ((CreatedbStmt *) parsetree)->options)
				{
					DefElem	   *defel = (DefElem *) lfirst(cell);

					if (strcmp(defel->defname, "template") == 0)
					{
						sepgsql_context_info.createdb_dtemplate
							= strVal(defel->arg);
						break;
					}
				}
				break;

			case T_LoadStmt:
				/*
				 * We reject LOAD command across the board on enforcing mode,
				 * because a binary module can arbitrarily override hooks.
				 */
				if (sepgsql_getenforce())
				{
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("SELinux: LOAD is not permitted")));
				}
				break;
			default:
				/*
				 * Right now we don't check any other utility commands,
				 * because it needs more detailed information to make access
				 * control decision here, but we don't want to have two parse
				 * and analyze routines individually.
				 */
				break;
		}

		if (next_ProcessUtility_hook)
			(*next_ProcessUtility_hook) (parsetree, queryString, params,
										 isTopLevel, dest, completionTag);
		else
			standard_ProcessUtility(parsetree, queryString, params,
									isTopLevel, dest, completionTag);
	}
	PG_CATCH();
	{
		sepgsql_context_info = saved_context_info;
		PG_RE_THROW();
	}
	PG_END_TRY();
	sepgsql_context_info = saved_context_info;
}

/*
 * Module load/unload callback
 */
void
_PG_init(void)
{
	char	   *context;

	/*
	 * We allow to load the SE-PostgreSQL module on single-user-mode or
	 * shared_preload_libraries settings only.
	 */
	if (IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("sepgsql must be loaded via shared_preload_libraries")));

	/*
	 * Check availability of SELinux on the platform. If disabled, we cannot
	 * activate any SE-PostgreSQL features, and we have to skip rest of
	 * initialization.
	 */
	if (is_selinux_enabled() < 1)
	{
		sepgsql_set_mode(SEPGSQL_MODE_DISABLED);
		return;
	}

	/*
	 * sepgsql.permissive = (on|off)
	 *
	 * This variable controls performing mode of SE-PostgreSQL on user's
	 * session.
	 */
	DefineCustomBoolVariable("sepgsql.permissive",
							 "Turn on/off permissive mode in SE-PostgreSQL",
							 NULL,
							 &sepgsql_permissive,
							 false,
							 PGC_SIGHUP,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * sepgsql.debug_audit = (on|off)
	 *
	 * This variable allows users to turn on/off audit logs on access control
	 * decisions, independent from auditallow/auditdeny setting in the
	 * security policy. We intend to use this option for debugging purpose.
	 */
	DefineCustomBoolVariable("sepgsql.debug_audit",
							 "Turn on/off debug audit messages",
							 NULL,
							 &sepgsql_debug_audit,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * Set up dummy client label.
	 *
	 * XXX - note that PostgreSQL launches background worker process like
	 * autovacuum without authentication steps. So, we initialize sepgsql_mode
	 * with SEPGSQL_MODE_INTERNAL, and client_label with the security context
	 * of server process. Later, it also launches background of user session.
	 * In this case, the process is always hooked on post-authentication, and
	 * we can initialize the sepgsql_mode and client_label correctly.
	 */
	if (getcon_raw(&context) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: failed to get server security label: %m")));
	sepgsql_set_client_label(context);

	/* Initialize userspace access vector cache */
	sepgsql_avc_init();

	/* Security label provider hook */
	register_label_provider(SEPGSQL_LABEL_TAG,
							sepgsql_object_relabel);

	/* Client authentication hook */
	next_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = sepgsql_client_auth;

	/* Object access hook */
	next_object_access_hook = object_access_hook;
	object_access_hook = sepgsql_object_access;

	/* DML permission check */
	next_exec_check_perms_hook = ExecutorCheckPerms_hook;
	ExecutorCheckPerms_hook = sepgsql_exec_check_perms;

	/* Trusted procedure hooks */
	next_needs_fmgr_hook = needs_fmgr_hook;
	needs_fmgr_hook = sepgsql_needs_fmgr_hook;

	next_fmgr_hook = fmgr_hook;
	fmgr_hook = sepgsql_fmgr_hook;

	/* ProcessUtility hook */
	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = sepgsql_utility_command;

	/* ExecutorStart hook */
	next_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = sepgsql_executor_start;

	/* init contextual info */
	memset(&sepgsql_context_info, 0, sizeof(sepgsql_context_info));
}
