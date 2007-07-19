/* security.cc: NT security functions

   Copyright 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005,
   2006, 2007 Red Hat, Inc.

   Originaly written by Gunther Ebert, gunther.ebert@ixos-leipzig.de
   Completely rewritten by Corinna Vinschen <corinna@vinschen.de>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <ctype.h>
#include <winnls.h>
#include <wingdi.h>
#include <winuser.h>
#include <wininet.h>
#include <ntsecapi.h>
#include <aclapi.h>
#include <dsgetdc.h>
#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "pinfo.h"
#include "cygheap.h"
#include <ntdef.h>
#include "ntdll.h"
#include "lm.h"
#include "pwdgrp.h"
#include "cyglsa.h"
#include <cygwin/version.h>

/* Set ntsec explicit as default. */
bool allow_ntsec = true;
/* allow_smbntsec is handled exclusively in path.cc (path_conv::check).
   It's defined here because of it's strong relationship to allow_ntsec. */
bool allow_smbntsec;

extern "C" void
cygwin_set_impersonation_token (const HANDLE hToken)
{
  debug_printf ("set_impersonation_token (%d)", hToken);
  cygheap->user.external_token = hToken == INVALID_HANDLE_VALUE ? NO_IMPERSONATION : hToken;
}

void
extract_nt_dom_user (const struct passwd *pw, char *domain, char *user)
{
  char *d, *u, *c;

  domain[0] = 0;
  strlcpy (user, pw->pw_name, UNLEN + 1);
  debug_printf ("pw_gecos %x (%s)", pw->pw_gecos, pw->pw_gecos);

  if ((d = strstr (pw->pw_gecos, "U-")) != NULL &&
      (d == pw->pw_gecos || d[-1] == ','))
    {
      c = strechr (d + 2, ',');
      if ((u = strechr (d + 2, '\\')) >= c)
	u = d + 1;
      else if (u - d <= INTERNET_MAX_HOST_NAME_LENGTH + 2)
	strlcpy (domain, d + 2, u - d - 1);
      if (c - u <= UNLEN + 1)
	strlcpy (user, u + 1, c - u);
    }
  if (domain[0])
    return;

  cygsid psid;
  DWORD ulen = UNLEN + 1;
  DWORD dlen = INTERNET_MAX_HOST_NAME_LENGTH + 1;
  SID_NAME_USE use;
  if (psid.getfrompw (pw))
    LookupAccountSid (NULL, psid, user, &ulen, domain, &dlen, &use);
}

extern "C" HANDLE
cygwin_logon_user (const struct passwd *pw, const char *password)
{
  if (!pw)
    {
      set_errno (EINVAL);
      return INVALID_HANDLE_VALUE;
    }

  char nt_domain[INTERNET_MAX_HOST_NAME_LENGTH + 1];
  char nt_user[UNLEN + 1];
  HANDLE hToken;

  extract_nt_dom_user (pw, nt_domain, nt_user);
  debug_printf ("LogonUserA (%s, %s, %s, ...)", nt_user, nt_domain, password);
  /* CV 2005-06-08: LogonUser should run under the primary process token,
     otherwise it returns with ERROR_ACCESS_DENIED on W2K. Don't ask me why. */
  RevertToSelf ();
  if (!LogonUserA (nt_user, *nt_domain ? nt_domain : NULL, (char *) password,
		   LOGON32_LOGON_INTERACTIVE,
		   LOGON32_PROVIDER_DEFAULT,
		   &hToken))
    {
      __seterrno ();
      hToken = INVALID_HANDLE_VALUE;
    }
  else if (!SetHandleInformation (hToken,
				  HANDLE_FLAG_INHERIT,
				  HANDLE_FLAG_INHERIT))
    {
      __seterrno ();
      CloseHandle (hToken);
      hToken = INVALID_HANDLE_VALUE;
    }
  cygheap->user.reimpersonate ();
  debug_printf ("%d = logon_user(%s,...)", hToken, pw->pw_name);
  return hToken;
}

static void
str2lsa (LSA_STRING &tgt, const char *srcstr)
{
  tgt.Length = strlen (srcstr);
  tgt.MaximumLength = tgt.Length + 1;
  tgt.Buffer = (PCHAR) srcstr;
}

static void
str2buf2lsa (LSA_STRING &tgt, char *buf, const char *srcstr)
{
  tgt.Length = strlen (srcstr);
  tgt.MaximumLength = tgt.Length + 1;
  tgt.Buffer = (PCHAR) buf;
  memcpy (buf, srcstr, tgt.MaximumLength);
}

/* The dimension of buf is assumed to be at least strlen(srcstr) + 1,
   The result will be shorter if the input has multibyte chars */
void
str2buf2uni (UNICODE_STRING &tgt, WCHAR *buf, const char *srcstr)
{
  tgt.Buffer = (PWCHAR) buf;
  tgt.MaximumLength = (strlen (srcstr) + 1) * sizeof (WCHAR);
  tgt.Length = sys_mbstowcs (buf, srcstr, tgt.MaximumLength / sizeof (WCHAR))
	       * sizeof (WCHAR);
  if (tgt.Length)
    tgt.Length -= sizeof (WCHAR);
}

void
str2uni_cat (UNICODE_STRING &tgt, const char *srcstr)
{
  int len = sys_mbstowcs (tgt.Buffer + tgt.Length / sizeof (WCHAR), srcstr,
			  (tgt.MaximumLength - tgt.Length) / sizeof (WCHAR));
  if (len)
    tgt.Length += (len - 1) * sizeof (WCHAR);
  else
    tgt.Length = tgt.MaximumLength = 0;
}

static LSA_HANDLE
open_local_policy ()
{
  LSA_OBJECT_ATTRIBUTES oa = { 0, 0, 0, 0, 0, 0 };
  LSA_HANDLE lsa = INVALID_HANDLE_VALUE;

  NTSTATUS ret = LsaOpenPolicy (NULL, &oa, POLICY_EXECUTE, &lsa);
  if (ret != STATUS_SUCCESS)
    __seterrno_from_win_error (LsaNtStatusToWinError (ret));
  return lsa;
}

static void
close_local_policy (LSA_HANDLE &lsa)
{
  if (lsa != INVALID_HANDLE_VALUE)
    LsaClose (lsa);
  lsa = INVALID_HANDLE_VALUE;
}

/* CV, 2006-07-06: Missing in w32api. */
extern "C" DWORD WINAPI DsGetDcNameA (LPCSTR, LPCSTR, GUID *, LPCSTR, ULONG,
				      PDOMAIN_CONTROLLER_INFOA *);
#define DS_FORCE_REDISCOVERY	1

bool
get_logon_server (const char *domain, char *server, WCHAR *wserver,
		  bool rediscovery)
{
  DWORD dret;
  PDOMAIN_CONTROLLER_INFOA pci;
  WCHAR *buf;
  DWORD size = INTERNET_MAX_HOST_NAME_LENGTH + 1;
  WCHAR wdomain[size];

  /* Empty domain is interpreted as local system */
  if ((GetComputerName (server + 2, &size)) &&
      (strcasematch (domain, server + 2) || !domain[0]))
    {
      server[0] = server[1] = '\\';
      if (wserver)
	sys_mbstowcs (wserver, server, INTERNET_MAX_HOST_NAME_LENGTH + 1);
      return true;
    }

  /* Try to get any available domain controller for this domain */
  dret = DsGetDcNameA (NULL, domain, NULL, NULL,
		       rediscovery ? DS_FORCE_REDISCOVERY : 0, &pci);
  if (dret == ERROR_SUCCESS)
    {
      strcpy (server, pci->DomainControllerName);
      sys_mbstowcs (wserver, server, INTERNET_MAX_HOST_NAME_LENGTH + 1);
      NetApiBufferFree (pci);
      debug_printf ("DC: rediscovery: %d, server: %s", rediscovery, server);
      return true;
    }
  else if (dret == ERROR_PROC_NOT_FOUND)
    {
      /* NT4 w/o DSClient */
      sys_mbstowcs (wdomain, domain, INTERNET_MAX_HOST_NAME_LENGTH + 1);
      if (rediscovery)
	dret = NetGetAnyDCName (NULL, wdomain, (LPBYTE *) &buf);
      else
	dret = NetGetDCName (NULL, wdomain, (LPBYTE *) &buf);
      if (dret == NERR_Success)
	{
	  sys_wcstombs (server, INTERNET_MAX_HOST_NAME_LENGTH + 1, buf);
	  if (wserver)
	    for (WCHAR *ptr1 = buf; (*wserver++ = *ptr1++);)
	      ;
	  NetApiBufferFree (buf);
	  debug_printf ("NT: rediscovery: %d, server: %s", rediscovery, server);
	  return true;
	}
    }
  __seterrno_from_win_error (dret);
  return false;
}

static bool
get_user_groups (WCHAR *wlogonserver, cygsidlist &grp_list, char *user,
		 char *domain)
{
  char dgroup[INTERNET_MAX_HOST_NAME_LENGTH + GNLEN + 2];
  WCHAR wuser[UNLEN + 1];
  sys_mbstowcs (wuser, user, UNLEN + 1);
  LPGROUP_USERS_INFO_0 buf;
  DWORD cnt, tot, len;
  NET_API_STATUS ret;

  /* Look only on logonserver */
  ret = NetUserGetGroups (wlogonserver, wuser, 0, (LPBYTE *) &buf,
			  MAX_PREFERRED_LENGTH, &cnt, &tot);
  if (ret)
    {
      __seterrno_from_win_error (ret);
      /* It's no error when the user name can't be found. */
      return ret == NERR_UserNotFound;
    }

  len = strlen (domain);
  strcpy (dgroup, domain);
  dgroup[len++] = '\\';

  for (DWORD i = 0; i < cnt; ++i)
    {
      cygsid gsid;
      DWORD glen = MAX_SID_LEN;
      char domain[INTERNET_MAX_HOST_NAME_LENGTH + 1];
      DWORD dlen = sizeof (domain);
      SID_NAME_USE use = SidTypeInvalid;

      sys_wcstombs (dgroup + len, GNLEN + 1, buf[i].grui0_name);
      if (!LookupAccountName (NULL, dgroup, gsid, &glen, domain, &dlen, &use))
	debug_printf ("LookupAccountName(%s), %E", dgroup);
      else if (legal_sid_type (use))
	grp_list += gsid;
      else
	debug_printf ("Global group %s invalid. Domain: %s Use: %d",
		      dgroup, domain, use);
    }

  NetApiBufferFree (buf);
  return true;
}

static bool
is_group_member (WCHAR *wgroup, PSID pusersid, cygsidlist &grp_list)
{
  LPLOCALGROUP_MEMBERS_INFO_1 buf;
  DWORD cnt, tot;
  NET_API_STATUS ret;

  /* Members can be users or global groups */
  ret = NetLocalGroupGetMembers (NULL, wgroup, 1, (LPBYTE *) &buf,
				 MAX_PREFERRED_LENGTH, &cnt, &tot, NULL);
  if (ret)
    return false;

  bool retval = true;
  for (DWORD bidx = 0; bidx < cnt; ++bidx)
    if (EqualSid (pusersid, buf[bidx].lgrmi1_sid))
      goto done;
    else
      {
	/* The extra test for the group being a global group or a well-known
	   group is necessary, since apparently also aliases (for instance
	   Administrators or Users) can be members of local groups, even
	   though MSDN states otherwise.  The GUI refuses to put aliases into
	   local groups, but the CLI interface allows it.  However, a normal
	   logon token does not contain groups, in which the user is only
	   indirectly a member by being a member of an alias in this group.
	   So we also should not put them into the token group list.
	   Note: Allowing those groups in our group list renders external
	   tokens invalid, so that it becomes impossible to logon with
	   password and valid logon token. */
	for (int glidx = 0; glidx < grp_list.count (); ++glidx)
	  if ((buf[bidx].lgrmi1_sidusage == SidTypeGroup
	       || buf[bidx].lgrmi1_sidusage == SidTypeWellKnownGroup)
	      && EqualSid (grp_list.sids[glidx], buf[bidx].lgrmi1_sid))
	    goto done;
      }

  retval = false;
 done:
  NetApiBufferFree (buf);
  return retval;
}

static bool
get_user_local_groups (cygsidlist &grp_list, PSID pusersid)
{
  LPLOCALGROUP_INFO_0 buf;
  DWORD cnt, tot;
  NET_API_STATUS ret;

  ret = NetLocalGroupEnum (NULL, 0, (LPBYTE *) &buf,
			   MAX_PREFERRED_LENGTH, &cnt, &tot, NULL);
  if (ret)
    {
      __seterrno_from_win_error (ret);
      return false;
    }

  char bgroup[INTERNET_MAX_HOST_NAME_LENGTH + GNLEN + 2];
  char lgroup[INTERNET_MAX_HOST_NAME_LENGTH + GNLEN + 2];
  DWORD blen, llen;
  SID_NAME_USE use;

  blen = llen = INTERNET_MAX_HOST_NAME_LENGTH + 1;
  if (!LookupAccountSid (NULL, well_known_admins_sid, lgroup, &llen, bgroup, &blen, &use)
      || !GetComputerNameA (lgroup, &(llen = INTERNET_MAX_HOST_NAME_LENGTH + 1)))
    {
      __seterrno ();
      return false;
    }
  bgroup[blen++] = lgroup[llen++] = '\\';

  for (DWORD i = 0; i < cnt; ++i)
    if (is_group_member (buf[i].lgrpi0_name, pusersid, grp_list))
      {
	cygsid gsid;
	DWORD glen = MAX_SID_LEN;
	char domain[INTERNET_MAX_HOST_NAME_LENGTH + 1];
	DWORD dlen = sizeof (domain);

	use = SidTypeInvalid;
	sys_wcstombs (bgroup + blen, GNLEN + 1, buf[i].lgrpi0_name);
	if (!LookupAccountName (NULL, bgroup, gsid, &glen, domain, &dlen, &use))
	  {
	    if (GetLastError () != ERROR_NONE_MAPPED)
	      debug_printf ("LookupAccountName(%s), %E", bgroup);
	    strcpy (lgroup + llen, bgroup + blen);
	    if (!LookupAccountName (NULL, lgroup, gsid, &glen,
				    domain, &dlen, &use))
	      debug_printf ("LookupAccountName(%s), %E", lgroup);
	  }
	if (!legal_sid_type (use))
	  debug_printf ("Rejecting local %s. use: %d", bgroup + blen, use);
	grp_list *= gsid;
      }
  NetApiBufferFree (buf);
  return true;
}

static bool
sid_in_token_groups (PTOKEN_GROUPS grps, cygpsid sid)
{
  if (!grps)
    return false;
  for (DWORD i = 0; i < grps->GroupCount; ++i)
    if (sid == grps->Groups[i].Sid)
      return true;
  return false;
}

#if 0				/* Unused */
static BOOL
get_user_primary_group (WCHAR *wlogonserver, const char *user,
			PSID pusersid, cygsid &pgrpsid)
{
  LPUSER_INFO_3 buf;
  WCHAR wuser[UNLEN + 1];
  NET_API_STATUS ret;
  BOOL retval = FALSE;
  UCHAR count = 0;

  if (well_known_system_sid == pusersid)
    {
      pgrpsid = well_known_system_sid;
      return TRUE;
    }

  sys_mbstowcs (wuser, user, UNLEN + 1);
  ret = NetUserGetInfo (wlogonserver, wuser, 3, (LPBYTE *) &buf);
  if (ret)
    {
      __seterrno_from_win_error (ret);
      return FALSE;
    }

  pgrpsid = pusersid;
  if (IsValidSid (pgrpsid)
      && (count = *GetSidSubAuthorityCount (pgrpsid)) > 1)
    {
      *GetSidSubAuthority (pgrpsid, count - 1) = buf->usri3_primary_group_id;
      retval = TRUE;
    }
  NetApiBufferFree (buf);
  return retval;
}
#endif

static void
get_unix_group_sidlist (struct passwd *pw, cygsidlist &grp_list)
{
  struct __group32 *gr;
  cygsid gsid;

  for (int gidx = 0; (gr = internal_getgrent (gidx)); ++gidx)
    {
      if (gr->gr_gid == (__gid32_t) pw->pw_gid)
	goto found;
      else if (gr->gr_mem)
	for (int gi = 0; gr->gr_mem[gi]; ++gi)
	  if (strcasematch (pw->pw_name, gr->gr_mem[gi]))
	    goto found;
      continue;
    found:
      if (gsid.getfromgr (gr))
	grp_list += gsid;

    }
}

static void
get_token_group_sidlist (cygsidlist &grp_list, PTOKEN_GROUPS my_grps,
			 LUID auth_luid, int &auth_pos)
{
  auth_pos = -1;
  if (my_grps)
    {
      grp_list += well_known_local_sid;
      if (sid_in_token_groups (my_grps, well_known_dialup_sid))
	grp_list *= well_known_dialup_sid;
      if (sid_in_token_groups (my_grps, well_known_network_sid))
	grp_list *= well_known_network_sid;
      if (sid_in_token_groups (my_grps, well_known_batch_sid))
	grp_list *= well_known_batch_sid;
      grp_list *= well_known_interactive_sid;
      if (sid_in_token_groups (my_grps, well_known_service_sid))
	grp_list *= well_known_service_sid;
      if (sid_in_token_groups (my_grps, well_known_this_org_sid))
	grp_list *= well_known_this_org_sid;
    }
  else
    {
      grp_list += well_known_local_sid;
      grp_list *= well_known_interactive_sid;
    }
  if (get_ll (auth_luid) != 999LL) /* != SYSTEM_LUID */
    {
      for (DWORD i = 0; i < my_grps->GroupCount; ++i)
	if (my_grps->Groups[i].Attributes & SE_GROUP_LOGON_ID)
	  {
	    grp_list += my_grps->Groups[i].Sid;
	    auth_pos = grp_list.count () - 1;
	    break;
	  }
    }
}

bool
get_server_groups (cygsidlist &grp_list, PSID usersid, struct passwd *pw)
{
  char user[UNLEN + 1];
  char domain[INTERNET_MAX_HOST_NAME_LENGTH + 1];
  WCHAR wserver[INTERNET_MAX_HOST_NAME_LENGTH + 3];
  char server[INTERNET_MAX_HOST_NAME_LENGTH + 3];

  if (well_known_system_sid == usersid)
    {
      grp_list *= well_known_admins_sid;
      get_unix_group_sidlist (pw, grp_list);
      return true;
    }

  grp_list *= well_known_world_sid;
  grp_list *= well_known_authenticated_users_sid;
  extract_nt_dom_user (pw, domain, user);
  if (get_logon_server (domain, server, wserver, false)
      && !get_user_groups (wserver, grp_list, user, domain)
      && get_logon_server (domain, server, wserver, true))
    get_user_groups (wserver, grp_list, user, domain);
  if (get_user_local_groups (grp_list, usersid))
    {
      get_unix_group_sidlist (pw, grp_list);
      return true;
    }
  return false;
}

static bool
get_initgroups_sidlist (cygsidlist &grp_list,
			PSID usersid, PSID pgrpsid, struct passwd *pw,
			PTOKEN_GROUPS my_grps, LUID auth_luid, int &auth_pos)
{
  grp_list *= well_known_world_sid;
  grp_list *= well_known_authenticated_users_sid;
  if (well_known_system_sid == usersid)
    auth_pos = -1;
  else
    get_token_group_sidlist (grp_list, my_grps, auth_luid, auth_pos);
  if (!get_server_groups (grp_list, usersid, pw))
    return false;

  /* special_pgrp true if pgrpsid is not in normal groups */
  grp_list += pgrpsid;
  return true;
}

static void
get_setgroups_sidlist (cygsidlist &tmp_list, PSID usersid, struct passwd *pw,
		       PTOKEN_GROUPS my_grps, user_groups &groups,
		       LUID auth_luid, int &auth_pos)
{
  tmp_list *= well_known_world_sid;
  tmp_list *= well_known_authenticated_users_sid;
  get_token_group_sidlist (tmp_list, my_grps, auth_luid, auth_pos);
  get_server_groups (tmp_list, usersid, pw);
  for (int gidx = 0; gidx < groups.sgsids.count (); gidx++)
    tmp_list += groups.sgsids.sids[gidx];
  tmp_list += groups.pgsid;
}

static ULONG sys_privs[] = {
  SE_CREATE_TOKEN_PRIVILEGE,
  SE_ASSIGNPRIMARYTOKEN_PRIVILEGE,		
  SE_LOCK_MEMORY_PRIVILEGE,		
  SE_INCREASE_QUOTA_PRIVILEGE,		
  SE_TCB_PRIVILEGE,		
  SE_SECURITY_PRIVILEGE,		
  SE_TAKE_OWNERSHIP_PRIVILEGE,		
  SE_LOAD_DRIVER_PRIVILEGE,		
  SE_SYSTEM_PROFILE_PRIVILEGE,		/* Vista ONLY */
  SE_SYSTEMTIME_PRIVILEGE,		
  SE_PROF_SINGLE_PROCESS_PRIVILEGE,		
  SE_INC_BASE_PRIORITY_PRIVILEGE,		
  SE_CREATE_PAGEFILE_PRIVILEGE,		
  SE_CREATE_PERMANENT_PRIVILEGE,		
  SE_BACKUP_PRIVILEGE,		
  SE_RESTORE_PRIVILEGE,		
  SE_SHUTDOWN_PRIVILEGE,		
  SE_DEBUG_PRIVILEGE,		
  SE_AUDIT_PRIVILEGE,		
  SE_SYSTEM_ENVIRONMENT_PRIVILEGE,		
  SE_CHANGE_NOTIFY_PRIVILEGE,		
  SE_UNDOCK_PRIVILEGE,
  SE_MANAGE_VOLUME_PRIVILEGE,
  SE_IMPERSONATE_PRIVILEGE,
  SE_CREATE_GLOBAL_PRIVILEGE,
  SE_INCREASE_WORKING_SET_PRIVILEGE,
  SE_TIME_ZONE_PRIVILEGE,
  SE_CREATE_SYMBOLIC_LINK_PRIVILEGE
};

#define SYSTEM_PRIVILEGES_COUNT (sizeof sys_privs / sizeof *sys_privs)

static PTOKEN_PRIVILEGES
get_system_priv_list (size_t &size)
{
  ULONG max_idx = 0;
  while (max_idx < SYSTEM_PRIVILEGES_COUNT
  	 && sys_privs[max_idx] != wincap.max_sys_priv ())
    ++max_idx;
  if (max_idx >= SYSTEM_PRIVILEGES_COUNT)
    api_fatal ("Coding error: wincap privilege %u doesn't exist in sys_privs",
	       wincap.max_sys_priv ());
  size = sizeof (ULONG) + (max_idx + 1) * sizeof (LUID_AND_ATTRIBUTES);
  PTOKEN_PRIVILEGES privs = (PTOKEN_PRIVILEGES) malloc (size);
  if (!privs)
    {
      debug_printf ("malloc (system_privs) failed.");
      return NULL;
    }
  privs->PrivilegeCount = 0;
  for (ULONG i = 0; i <= max_idx; ++i)
    {
      privs->Privileges[privs->PrivilegeCount].Luid.HighPart = 0L;
      privs->Privileges[privs->PrivilegeCount].Luid.LowPart = sys_privs[i];
      privs->Privileges[privs->PrivilegeCount].Attributes =
	SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;
      ++privs->PrivilegeCount;
    }
  return privs;
}

static PTOKEN_PRIVILEGES
get_priv_list (LSA_HANDLE lsa, cygsid &usersid, cygsidlist &grp_list,
	       size_t &size)
{
  PLSA_UNICODE_STRING privstrs;
  ULONG cnt;
  PTOKEN_PRIVILEGES privs = NULL;
  NTSTATUS ret;
  char buf[INTERNET_MAX_HOST_NAME_LENGTH + 1];

  if (usersid == well_known_system_sid)
    return get_system_priv_list (size);

  for (int grp = -1; grp < grp_list.count (); ++grp)
    {
      if (grp == -1)
	{
	  if ((ret = LsaEnumerateAccountRights (lsa, usersid, &privstrs,
						&cnt)) != STATUS_SUCCESS)
	    continue;
	}
      else if ((ret = LsaEnumerateAccountRights (lsa, grp_list.sids[grp],
						 &privstrs, &cnt))
	       != STATUS_SUCCESS)
	continue;
      for (ULONG i = 0; i < cnt; ++i)
	{
	  LUID priv;
	  PTOKEN_PRIVILEGES tmp;
	  DWORD tmp_count;

	  sys_wcstombs (buf, sizeof (buf),
			privstrs[i].Buffer, privstrs[i].Length / 2);
	  if (!privilege_luid (buf, &priv))
	    continue;

	  if (privs)
	    {
	      DWORD pcnt = privs->PrivilegeCount;
	      LUID_AND_ATTRIBUTES *p = privs->Privileges;
	      for (; pcnt > 0; --pcnt, ++p)
		if (priv.HighPart == p->Luid.HighPart
		    && priv.LowPart == p->Luid.LowPart)
		  goto next_account_right;
	    }

	  tmp_count = privs ? privs->PrivilegeCount : 0;
	  size = sizeof (DWORD)
		 + (tmp_count + 1) * sizeof (LUID_AND_ATTRIBUTES);
	  tmp = (PTOKEN_PRIVILEGES) realloc (privs, size);
	  if (!tmp)
	    {
	      if (privs)
		free (privs);
	      LsaFreeMemory (privstrs);
	      debug_printf ("realloc (privs) failed.");
	      return NULL;
	    }
	  tmp->PrivilegeCount = tmp_count;
	  privs = tmp;
	  privs->Privileges[privs->PrivilegeCount].Luid = priv;
	  privs->Privileges[privs->PrivilegeCount].Attributes =
	    SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;
	  ++privs->PrivilegeCount;

	next_account_right:
	  ;
	}
      LsaFreeMemory (privstrs);
    }
  return privs;
}

/* Accept a token if
   - the requested usersid matches the TokenUser and
   - if setgroups has been called:
	the token groups that are listed in /etc/group match the union of
	the requested primary and supplementary groups in gsids.
   - else the (unknown) implicitly requested supplementary groups and those
	in the token are the groups associated with the usersid. We assume
	they match and verify only the primary groups.
	The requested primary group must appear in the token.
	The primary group in the token is a group associated with the usersid,
	except if the token is internal and the group is in the token SD
	(see create_token). In that latter case that group must match the
	requested primary group.  */
bool
verify_token (HANDLE token, cygsid &usersid, user_groups &groups, bool *pintern)
{
  DWORD size;
  bool intern = false;

  if (pintern)
    {
      TOKEN_SOURCE ts;
      if (!GetTokenInformation (token, TokenSource,
				&ts, sizeof ts, &size))
	debug_printf ("GetTokenInformation(), %E");
      else
	*pintern = intern = !memcmp (ts.SourceName, "Cygwin.1", 8);
    }
  /* Verify usersid */
  cygsid tok_usersid = NO_SID;
  if (!GetTokenInformation (token, TokenUser,
			    &tok_usersid, sizeof tok_usersid, &size))
    debug_printf ("GetTokenInformation(), %E");
  if (usersid != tok_usersid)
    return false;

  /* For an internal token, if setgroups was not called and if the sd group
     is not well_known_null_sid, it must match pgrpsid */
  if (intern && !groups.issetgroups ())
    {
      const DWORD sd_buf_siz = MAX_SID_LEN + sizeof (SECURITY_DESCRIPTOR);
      PSECURITY_DESCRIPTOR sd_buf = (PSECURITY_DESCRIPTOR) alloca (sd_buf_siz);
      cygpsid gsid (NO_SID);
      if (!GetKernelObjectSecurity (token, GROUP_SECURITY_INFORMATION,
				    sd_buf, sd_buf_siz, &size))
	debug_printf ("GetKernelObjectSecurity(), %E");
      else if (!GetSecurityDescriptorGroup (sd_buf, (PSID *) &gsid,
					    (BOOL *) &size))
	debug_printf ("GetSecurityDescriptorGroup(), %E");
      if (well_known_null_sid != gsid)
	return gsid == groups.pgsid;
    }

  PTOKEN_GROUPS my_grps;
  bool sawpg = false, ret = false;

  if (!GetTokenInformation (token, TokenGroups, NULL, 0, &size) &&
      GetLastError () != ERROR_INSUFFICIENT_BUFFER)
    debug_printf ("GetTokenInformation(token, TokenGroups), %E");
  else if (!(my_grps = (PTOKEN_GROUPS) alloca (size)))
    debug_printf ("alloca (my_grps) failed.");
  else if (!GetTokenInformation (token, TokenGroups, my_grps, size, &size))
    debug_printf ("GetTokenInformation(my_token, TokenGroups), %E");
  else
    {
      if (groups.issetgroups ()) /* setgroups was called */
	{
	  cygsid gsid;
	  struct __group32 *gr;
	  bool saw[groups.sgsids.count ()];
	  memset (saw, 0, sizeof(saw));

	  /* token groups found in /etc/group match the user.gsids ? */
	  for (int gidx = 0; (gr = internal_getgrent (gidx)); ++gidx)
	    if (gsid.getfromgr (gr) && sid_in_token_groups (my_grps, gsid))
	      {
		int pos = groups.sgsids.position (gsid);
		if (pos >= 0)
		  saw[pos] = true;
		else if (groups.pgsid == gsid)
		  sawpg = true;
		else if (gsid != well_known_world_sid
			 && gsid != usersid)
		  goto done;
	      }
	  /* user.sgsids groups must be in the token */
	  for (int gidx = 0; gidx < groups.sgsids.count (); gidx++)
	    if (!saw[gidx] && !sid_in_token_groups (my_grps, groups.sgsids.sids[gidx]))
	      goto done;
	}
      /* The primary group must be in the token */
      ret = sawpg
	|| sid_in_token_groups (my_grps, groups.pgsid)
	|| groups.pgsid == usersid;
    }
done:
  return ret;
}

HANDLE
create_token (cygsid &usersid, user_groups &new_groups, struct passwd *pw)
{
  NTSTATUS ret;
  LSA_HANDLE lsa = INVALID_HANDLE_VALUE;

  cygsidlist tmp_gsids (cygsidlist_auto, 12);

  SECURITY_QUALITY_OF_SERVICE sqos =
    { sizeof sqos, SecurityImpersonation, SECURITY_STATIC_TRACKING, FALSE };
  OBJECT_ATTRIBUTES oa = { sizeof oa, 0, 0, 0, 0, &sqos };
  LUID auth_luid = SYSTEM_LUID;
  LARGE_INTEGER exp = { QuadPart:INT64_MAX };

  TOKEN_USER user;
  PTOKEN_GROUPS new_tok_gsids = NULL;
  PTOKEN_PRIVILEGES privs = NULL;
  TOKEN_OWNER owner;
  TOKEN_PRIMARY_GROUP pgrp;
  TOKEN_DEFAULT_DACL dacl = {};
  TOKEN_SOURCE source;
  TOKEN_STATISTICS stats;
  memcpy (source.SourceName, "Cygwin.1", 8);
  source.SourceIdentifier.HighPart = 0;
  source.SourceIdentifier.LowPart = 0x0101;

  HANDLE token = INVALID_HANDLE_VALUE;
  HANDLE primary_token = INVALID_HANDLE_VALUE;

  PTOKEN_GROUPS my_tok_gsids = NULL;
  DWORD size;
  size_t psize = 0;

  /* SE_CREATE_TOKEN_NAME privilege needed to call NtCreateToken. */
  push_self_privilege (SE_CREATE_TOKEN_PRIVILEGE, true);

  /* Open policy object. */
  if ((lsa = open_local_policy ()) == INVALID_HANDLE_VALUE)
    goto out;

  /* User, owner, primary group. */
  user.User.Sid = usersid;
  user.User.Attributes = 0;
  owner.Owner = usersid;

  /* Retrieve authentication id and group list from own process. */
  if (hProcToken)
    {
      /* Switching user context to SYSTEM doesn't inherit the authentication
	 id of the user account running current process. */
      if (usersid != well_known_system_sid)
	if (!GetTokenInformation (hProcToken, TokenStatistics,
				  &stats, sizeof stats, &size))
	  debug_printf
	    ("GetTokenInformation(hProcToken, TokenStatistics), %E");
	else
	  auth_luid = stats.AuthenticationId;

      /* Retrieving current processes group list to be able to inherit
	 some important well known group sids. */
      if (!GetTokenInformation (hProcToken, TokenGroups, NULL, 0, &size)
	  && GetLastError () != ERROR_INSUFFICIENT_BUFFER)
	debug_printf ("GetTokenInformation(hProcToken, TokenGroups), %E");
      else if (!(my_tok_gsids = (PTOKEN_GROUPS) malloc (size)))
	debug_printf ("malloc (my_tok_gsids) failed.");
      else if (!GetTokenInformation (hProcToken, TokenGroups, my_tok_gsids,
				     size, &size))
	{
	  debug_printf ("GetTokenInformation(hProcToken, TokenGroups), %E");
	  free (my_tok_gsids);
	  my_tok_gsids = NULL;
	}
    }

  /* Create list of groups, the user is member in. */
  int auth_pos;
  if (new_groups.issetgroups ())
    get_setgroups_sidlist (tmp_gsids, usersid, pw, my_tok_gsids, new_groups,
			   auth_luid, auth_pos);
  else if (!get_initgroups_sidlist (tmp_gsids, usersid, new_groups.pgsid, pw,
				    my_tok_gsids, auth_luid, auth_pos))
    goto out;

  /* Primary group. */
  pgrp.PrimaryGroup = new_groups.pgsid;

  /* Create a TOKEN_GROUPS list from the above retrieved list of sids. */
  new_tok_gsids = (PTOKEN_GROUPS)
		  alloca (sizeof (DWORD) + tmp_gsids.count ()
					   * sizeof (SID_AND_ATTRIBUTES));
  new_tok_gsids->GroupCount = tmp_gsids.count ();
  for (DWORD i = 0; i < new_tok_gsids->GroupCount; ++i)
    {
      new_tok_gsids->Groups[i].Sid = tmp_gsids.sids[i];
      new_tok_gsids->Groups[i].Attributes = SE_GROUP_MANDATORY
					    | SE_GROUP_ENABLED_BY_DEFAULT
					    | SE_GROUP_ENABLED;
    }
  if (auth_pos >= 0)
    new_tok_gsids->Groups[auth_pos].Attributes |= SE_GROUP_LOGON_ID;
  /* Retrieve list of privileges of that user. */
  if (!(privs = get_priv_list (lsa, usersid, tmp_gsids, psize)))
    goto out;

  /* Let's be heroic... */
  ret = NtCreateToken (&token, TOKEN_ALL_ACCESS, &oa, TokenImpersonation,
		       &auth_luid, &exp, &user, new_tok_gsids, privs, &owner,
		       &pgrp, &dacl, &source);
  if (ret)
    __seterrno_from_nt_status (ret);
  else if (GetLastError () == ERROR_PROC_NOT_FOUND)
    {
      __seterrno ();
      debug_printf ("Loading NtCreateToken failed.");
    }
  else
    {
      /* Convert to primary token. */
      if (!DuplicateTokenEx (token, MAXIMUM_ALLOWED, &sec_none,
			     SecurityImpersonation, TokenPrimary,
			     &primary_token))
	{
	  __seterrno ();
	  debug_printf ("DuplicateTokenEx %E");
	}
    }

out:
  pop_self_privilege ();
  if (token != INVALID_HANDLE_VALUE)
    CloseHandle (token);
  if (privs)
    free (privs);
  if (my_tok_gsids)
    free (my_tok_gsids);
  close_local_policy (lsa);

  debug_printf ("0x%x = create_token ()", primary_token);
  return primary_token;
}

HANDLE
lsaauth (cygsid &usersid, user_groups &new_groups, struct passwd *pw)
{
  cygsidlist tmp_gsids (cygsidlist_auto, 12);
  cygpsid pgrpsid;
  LSA_STRING name;
  HANDLE lsa_hdl = NULL, lsa = INVALID_HANDLE_VALUE;
  LSA_OPERATIONAL_MODE sec_mode;
  NTSTATUS ret, ret2;
  ULONG package_id, size;
  LUID auth_luid = SYSTEM_LUID;
  struct {
    LSA_STRING str;
    CHAR buf[16];
  } origin;
  cyglsa_t *authinf = NULL;
  ULONG authinf_size;
  TOKEN_SOURCE ts;
  PCYG_TOKEN_GROUPS gsids = NULL;
  PTOKEN_PRIVILEGES privs = NULL;
  PACL dacl = NULL;
  PVOID profile = NULL;
  LUID luid;
  QUOTA_LIMITS quota;
  size_t psize = 0, gsize = 0, dsize = 0;
  OFFSET offset, sids_offset;
  int tmpidx, non_well_known_cnt;

  HANDLE user_token = NULL;

  push_self_privilege (SE_TCB_PRIVILEGE, true);

  /* Register as logon process. */
  str2lsa (name, "Cygwin");
  SetLastError (0);
  ret = LsaRegisterLogonProcess (&name, &lsa_hdl, &sec_mode);
  if (ret != STATUS_SUCCESS)
    {
      debug_printf ("LsaRegisterLogonProcess: %p", ret);
      __seterrno_from_win_error (LsaNtStatusToWinError (ret));
      goto out;
    }
  else if (GetLastError () == ERROR_PROC_NOT_FOUND)
    {
      debug_printf ("Couldn't load Secur32.dll");
      goto out;
    }
  /* Get handle to our own LSA package. */
  str2lsa (name, CYG_LSA_PKGNAME);
  ret = LsaLookupAuthenticationPackage (lsa_hdl, &name, &package_id);
  if (ret != STATUS_SUCCESS)
    {
      debug_printf ("LsaLookupAuthenticationPackage: %p", ret);
      __seterrno_from_win_error (LsaNtStatusToWinError (ret));
      goto out;
    }

  /* Open policy object. */
  if ((lsa = open_local_policy ()) == INVALID_HANDLE_VALUE)
    goto out;

  /* Create origin. */
  str2buf2lsa (origin.str, origin.buf, "Cygwin");
  /* Create token source. */
  memcpy (ts.SourceName, "Cygwin.1", 8);
  ts.SourceIdentifier.HighPart = 0;
  ts.SourceIdentifier.LowPart = 0x0103;

  /* Create list of groups, the user is member in. */
  int auth_pos;
  if (new_groups.issetgroups ())
    get_setgroups_sidlist (tmp_gsids, usersid, pw, NULL, new_groups, auth_luid,
			   auth_pos);
  else if (!get_initgroups_sidlist (tmp_gsids, usersid, new_groups.pgsid, pw,
				    NULL, auth_luid, auth_pos))
    goto out;
  /* The logon SID entry is not generated automatically on Windows 2000
     and earlier for some reason.  So add fake logon sid here, which is
     filled with logon id values in the authentication package. */
  if (wincap.needs_logon_sid_in_sid_list ())
    tmp_gsids += fake_logon_sid;

  tmp_gsids.debug_print ("tmp_gsids");

  /* Evaluate size of TOKEN_GROUPS list */
  non_well_known_cnt =  tmp_gsids.non_well_known_count ();
  gsize = sizeof (DWORD) + non_well_known_cnt * sizeof (SID_AND_ATTRIBUTES);
  tmpidx = -1;
  for (int i = 0; i < non_well_known_cnt; ++i)
    if ((tmpidx = tmp_gsids.next_non_well_known_sid (tmpidx)) >= 0)
      gsize += GetLengthSid (tmp_gsids.sids[tmpidx]);

  /* Retrieve list of privileges of that user. */
  if (!(privs = get_priv_list (lsa, usersid, tmp_gsids, psize)))
    goto out;

  /* Create DefaultDacl. */
  dsize = sizeof (ACL) + 3 * sizeof (ACCESS_ALLOWED_ACE)
	  + GetLengthSid (usersid)
	  + GetLengthSid (well_known_admins_sid)
	  + GetLengthSid (well_known_system_sid);
  dacl = (PACL) alloca (dsize);
  if (!InitializeAcl (dacl, dsize, ACL_REVISION))
    goto out;
  if (!AddAccessAllowedAce (dacl, ACL_REVISION, GENERIC_ALL, usersid))
    goto out;
  if (!AddAccessAllowedAce (dacl, ACL_REVISION, GENERIC_ALL,
			    well_known_admins_sid))
    goto out;
  if (!AddAccessAllowedAce (dacl, ACL_REVISION, GENERIC_ALL,
			    well_known_system_sid))
    goto out;

  /* Evaluate authinf size and allocate authinf. */
  authinf_size = (authinf->data - (PBYTE) authinf);
  authinf_size += GetLengthSid (usersid);	    /* User SID */
  authinf_size += gsize;			    /* Groups + Group SIDs */
  /* When trying to define the admins group as primary group on Vista,
     LsaLogonUser fails with error STATUS_INVALID_OWNER.  As workaround
     we define "Local" as primary group here.  First, this adds the otherwise
     missing "Local" group to the group list and second, seteuid32
     sets the primary group to the group set in /etc/passwd anyway. */
  pgrpsid = well_known_local_sid;
  authinf_size += GetLengthSid (pgrpsid);	    /* Primary Group SID */

  authinf_size += psize;			    /* Privileges */
  authinf_size += 0;				    /* Owner SID */
  authinf_size += dsize;			    /* Default DACL */

  authinf = (cyglsa_t *) alloca (authinf_size);
  authinf->inf_size = authinf_size - ((PBYTE) &authinf->inf - (PBYTE) authinf);

  authinf->magic = CYG_LSA_MAGIC;

  extract_nt_dom_user (pw, authinf->domain, authinf->username);

  /* Store stuff in authinf with offset relative to start of "inf" member,
     instead of using pointers. */
  offset = authinf->data - (PBYTE) &authinf->inf;

  authinf->inf.ExpirationTime.LowPart = 0xffffffffL;
  authinf->inf.ExpirationTime.HighPart = 0x7fffffffL;
  /* User SID */
  authinf->inf.User.User.Sid = offset;
  authinf->inf.User.User.Attributes = 0;
  CopySid (GetLengthSid (usersid), (PSID) ((PBYTE) &authinf->inf + offset),
	   usersid);
  offset += GetLengthSid (usersid);
  /* Groups */
  authinf->inf.Groups = offset;
  gsids = (PCYG_TOKEN_GROUPS) ((PBYTE) &authinf->inf + offset);
  sids_offset = offset + sizeof (ULONG) + non_well_known_cnt
					  * sizeof (SID_AND_ATTRIBUTES);
  gsids->GroupCount = non_well_known_cnt;
  /* Group SIDs */
  tmpidx = -1;
  for (int i = 0; i < non_well_known_cnt; ++i)
    {
      if ((tmpidx = tmp_gsids.next_non_well_known_sid (tmpidx)) < 0)
	break;
      gsids->Groups[i].Sid = sids_offset;
      gsids->Groups[i].Attributes = SE_GROUP_MANDATORY
				    | SE_GROUP_ENABLED_BY_DEFAULT
				    | SE_GROUP_ENABLED;
      /* Mark logon SID as logon SID :) */
      if (wincap.needs_logon_sid_in_sid_list ()
	  && tmp_gsids.sids[tmpidx] == fake_logon_sid)
	gsids->Groups[i].Attributes += SE_GROUP_LOGON_ID;
      CopySid (GetLengthSid (tmp_gsids.sids[tmpidx]),
	       (PSID) ((PBYTE) &authinf->inf + sids_offset),
	       tmp_gsids.sids[tmpidx]);
      sids_offset += GetLengthSid (tmp_gsids.sids[tmpidx]);
    }
  offset += gsize;
  /* Primary Group SID */
  authinf->inf.PrimaryGroup.PrimaryGroup = offset;
  CopySid (GetLengthSid (pgrpsid), (PSID) ((PBYTE) &authinf->inf + offset),
	   pgrpsid);
  offset += GetLengthSid (pgrpsid);
  /* Privileges */
  authinf->inf.Privileges = offset;
  memcpy ((PBYTE) &authinf->inf + offset, privs, psize);
  offset += psize;
  /* Owner */
  authinf->inf.Owner.Owner = 0;
  /* Default DACL */
  authinf->inf.DefaultDacl.DefaultDacl = offset;
  memcpy ((PBYTE) &authinf->inf + offset, dacl, dsize);

  authinf->checksum = CYGWIN_VERSION_MAGIC (CYGWIN_VERSION_DLL_MAJOR,
					    CYGWIN_VERSION_DLL_MINOR);
  PDWORD csp = (PDWORD) &authinf->username;
  PDWORD csp_end = (PDWORD) ((PBYTE) authinf + authinf_size);
  while (csp < csp_end)
    authinf->checksum += *csp++;

  /* Try to logon... */
  ret = LsaLogonUser (lsa_hdl, (PLSA_STRING) &origin, Interactive, package_id,
		      authinf, authinf_size, NULL, &ts, &profile, &size, &luid,
		      &user_token, &quota, &ret2);
  if (ret != STATUS_SUCCESS)
    {
      debug_printf ("LsaLogonUser: %p", ret);
      __seterrno_from_win_error (LsaNtStatusToWinError (ret));
      goto out;
    }
  if (profile)
    LsaFreeReturnBuffer (profile);

  if (wincap.has_mandatory_integrity_control ())
    {
      typedef struct _TOKEN_LINKED_TOKEN
      {
	HANDLE LinkedToken;
      } TOKEN_LINKED_TOKEN, *PTOKEN_LINKED_TOKEN;
#     define TokenLinkedToken ((TOKEN_INFORMATION_CLASS) 19)

      TOKEN_LINKED_TOKEN linked;

      if (GetTokenInformation (user_token, TokenLinkedToken,
			       (PVOID) &linked, sizeof linked, &size))
	{
	  debug_printf ("Linked Token: %lu", linked.LinkedToken);
	  if (linked.LinkedToken)
	    user_token = linked.LinkedToken;
	}
    }

out:
  if (privs)
    free (privs);
  close_local_policy (lsa);
  if (lsa_hdl)
    LsaDeregisterLogonProcess (lsa_hdl);
  pop_self_privilege ();

  debug_printf ("0x%x = lsaauth ()", user_token);
  return user_token;
}

/* read_sd reads a security descriptor from a file.
   In case of error, -1 is returned and errno is set.
   If sd_buf is too small, 0 is returned and sd_size
   is set to the needed buffer size.
   On success, 1 is returned.

   GetFileSecurity() is used instead of BackupRead()
   to avoid access denied errors if the caller has
   not the permission to open that file for read.

   Originally the function should return the size
   of the SD on success. Unfortunately NT returns
   0 in `len' on success, while W2K returns the
   correct size!

   2003-11-26: Now the function allocates the space needed by itself so
   it knows the real size and returns it in the security_descriptor object.
*/

LONG
read_sd (const char *file, security_descriptor &sd)
{

  DWORD len = 0;
  const char *pfile = file;
  char fbuf[CYG_MAX_PATH];
  if (current_codepage == oem_cp)
    {
      DWORD fname_len = min (sizeof (fbuf) - 1, strlen (file));
      bzero (fbuf, sizeof (fbuf));
      OemToCharBuff (file, fbuf, fname_len);
      pfile = fbuf;
    }

  if (!GetFileSecurity (pfile,
			OWNER_SECURITY_INFORMATION
			| GROUP_SECURITY_INFORMATION
			| DACL_SECURITY_INFORMATION,
			NULL, 0, &len)
      && GetLastError () != ERROR_INSUFFICIENT_BUFFER)
    {
      debug_printf ("file %s", file);
      __seterrno ();
      return -1;
    }
  debug_printf ("file %s: len %d", file, len);
  if (!sd.malloc (len))
    {
      set_errno (ENOMEM);
      return -1;
    }
  if (!GetFileSecurity (pfile,
			OWNER_SECURITY_INFORMATION
			| GROUP_SECURITY_INFORMATION
			| DACL_SECURITY_INFORMATION,
			sd, len, &len))
    {
      __seterrno ();
      return -1;
    }
  return sd.size ();
}

LONG
write_sd (HANDLE fh, const char *file, security_descriptor &sd)
{
  NTSTATUS ret = STATUS_SUCCESS;
  int retry = 0;
  int res = -1;
  for (; retry < 2; ++retry)
    {
      if (retry && (fh = CreateFile (file, WRITE_OWNER | WRITE_DAC,
				     FILE_SHARE_READ | FILE_SHARE_WRITE,
				     &sec_none_nih, OPEN_EXISTING,
				     FILE_ATTRIBUTE_NORMAL
				     | FILE_FLAG_BACKUP_SEMANTICS,
				     NULL)) == INVALID_HANDLE_VALUE)
	break;
      if (fh && (ret = NtSetSecurityObject (fh,
					    DACL_SECURITY_INFORMATION
					    | GROUP_SECURITY_INFORMATION
					    | OWNER_SECURITY_INFORMATION,
					    sd)) == STATUS_SUCCESS)
	break;
    }
  if (retry && fh != INVALID_HANDLE_VALUE)
    CloseHandle (fh);
  if (fh == INVALID_HANDLE_VALUE)	/* CreateFile failed */
    __seterrno ();
  else if (ret != STATUS_SUCCESS)	/* NtSetSecurityObject failed */
    __seterrno_from_nt_status (ret);
  else					/* Everything's fine. */
    res = 0;
  return res;
}

static void
get_attribute_from_acl (mode_t *attribute, PACL acl, PSID owner_sid,
			PSID group_sid, bool grp_member)
{
  ACCESS_ALLOWED_ACE *ace;
  int allow = 0;
  int deny = 0;
  int *flags, *anti;

  for (DWORD i = 0; i < acl->AceCount; ++i)
    {
      if (!GetAce (acl, i, (PVOID *) &ace))
	continue;
      if (ace->Header.AceFlags & INHERIT_ONLY)
	continue;
      switch (ace->Header.AceType)
	{
	case ACCESS_ALLOWED_ACE_TYPE:
	  flags = &allow;
	  anti = &deny;
	  break;
	case ACCESS_DENIED_ACE_TYPE:
	  flags = &deny;
	  anti = &allow;
	  break;
	default:
	  continue;
	}

      cygpsid ace_sid ((PSID) &ace->SidStart);
      if (ace_sid == well_known_world_sid)
	{
	  if (ace->Mask & FILE_READ_BITS)
	    *flags |= ((!(*anti & S_IROTH)) ? S_IROTH : 0)
		      | ((!(*anti & S_IRGRP)) ? S_IRGRP : 0)
		      | ((!(*anti & S_IRUSR)) ? S_IRUSR : 0);
	  if (ace->Mask & FILE_WRITE_BITS)
	    *flags |= ((!(*anti & S_IWOTH)) ? S_IWOTH : 0)
		      | ((!(*anti & S_IWGRP)) ? S_IWGRP : 0)
		      | ((!(*anti & S_IWUSR)) ? S_IWUSR : 0);
	  if (ace->Mask & FILE_EXEC_BITS)
	    *flags |= ((!(*anti & S_IXOTH)) ? S_IXOTH : 0)
		      | ((!(*anti & S_IXGRP)) ? S_IXGRP : 0)
		      | ((!(*anti & S_IXUSR)) ? S_IXUSR : 0);
	  if ((S_ISDIR (*attribute)) &&
	      (ace->Mask & (FILE_WRITE_DATA | FILE_EXECUTE | FILE_DELETE_CHILD))
	      == (FILE_WRITE_DATA | FILE_EXECUTE))
	    *flags |= S_ISVTX;
	}
      else if (ace_sid == well_known_null_sid)
	{
	  /* Read SUID, SGID and VTX bits from NULL ACE. */
	  if (ace->Mask & FILE_READ_DATA)
	    *flags |= S_ISVTX;
	  if (ace->Mask & FILE_WRITE_DATA)
	    *flags |= S_ISGID;
	  if (ace->Mask & FILE_APPEND_DATA)
	    *flags |= S_ISUID;
	}
      else if (ace_sid == owner_sid)
	{
	  if (ace->Mask & FILE_READ_BITS)
	    *flags |= ((!(*anti & S_IRUSR)) ? S_IRUSR : 0);
	  if (ace->Mask & FILE_WRITE_BITS)
	    *flags |= ((!(*anti & S_IWUSR)) ? S_IWUSR : 0);
	  if (ace->Mask & FILE_EXEC_BITS)
	    *flags |= ((!(*anti & S_IXUSR)) ? S_IXUSR : 0);
	}
      else if (ace_sid == group_sid)
	{
	  if (ace->Mask & FILE_READ_BITS)
	    *flags |= ((!(*anti & S_IRGRP)) ? S_IRGRP : 0)
		      | ((grp_member && !(*anti & S_IRUSR)) ? S_IRUSR : 0);
	  if (ace->Mask & FILE_WRITE_BITS)
	    *flags |= ((!(*anti & S_IWGRP)) ? S_IWGRP : 0)
		      | ((grp_member && !(*anti & S_IWUSR)) ? S_IWUSR : 0);
	  if (ace->Mask & FILE_EXEC_BITS)
	    *flags |= ((!(*anti & S_IXGRP)) ? S_IXGRP : 0)
		      | ((grp_member && !(*anti & S_IXUSR)) ? S_IXUSR : 0);
	}
    }
  *attribute &= ~(S_IRWXU | S_IRWXG | S_IRWXO | S_ISVTX | S_ISGID | S_ISUID);
  if (owner_sid && group_sid && EqualSid (owner_sid, group_sid)
      /* FIXME: temporary exception for /var/empty */
      && well_known_system_sid != group_sid)
    {
      allow &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
      allow |= (((allow & S_IRUSR) ? S_IRGRP : 0)
		| ((allow & S_IWUSR) ? S_IWGRP : 0)
		| ((allow & S_IXUSR) ? S_IXGRP : 0));
    }
  *attribute |= allow;
}

static void
get_info_from_sd (PSECURITY_DESCRIPTOR psd, mode_t *attribute,
		  __uid32_t *uidret, __gid32_t *gidret)
{
  if (!psd)
    {
      /* If reading the security descriptor failed, treat the object
	 as unreadable. */
      if (attribute)
	*attribute &= ~(S_IRWXU | S_IRWXG | S_IRWXO);
      if (uidret)
	*uidret = ILLEGAL_UID;
      if (gidret)
	*gidret = ILLEGAL_GID;
      return;
    }

  cygpsid owner_sid;
  cygpsid group_sid;
  BOOL dummy;

  if (!GetSecurityDescriptorOwner (psd, (PSID *) &owner_sid, &dummy))
    debug_printf ("GetSecurityDescriptorOwner %E");
  if (!GetSecurityDescriptorGroup (psd, (PSID *) &group_sid, &dummy))
    debug_printf ("GetSecurityDescriptorGroup %E");

  __uid32_t uid;
  __gid32_t gid;
  bool grp_member = get_sids_info (owner_sid, group_sid, &uid, &gid);
  if (uidret)
    *uidret = uid;
  if (gidret)
    *gidret = gid;

  if (!attribute)
    {
      syscall_printf ("uid %d, gid %d", uid, gid);
      return;
    }

  PACL acl;
  BOOL acl_exists;

  if (!GetSecurityDescriptorDacl (psd, &acl_exists, &acl, &dummy))
    {
      __seterrno ();
      debug_printf ("GetSecurityDescriptorDacl %E");
      *attribute &= ~(S_IRWXU | S_IRWXG | S_IRWXO);
    }
  else if (!acl_exists || !acl)
    *attribute |= S_IRWXU | S_IRWXG | S_IRWXO;
  else
    get_attribute_from_acl (attribute, acl, owner_sid, group_sid, grp_member);

  syscall_printf ("%sACL %x, uid %d, gid %d",
		  (!acl_exists || !acl)?"NO ":"", *attribute, uid, gid);
}

static int
get_reg_security (HANDLE handle, security_descriptor &sd_ret)
{
  LONG ret;
  DWORD len = 0;

  ret = RegGetKeySecurity ((HKEY) handle,
			   DACL_SECURITY_INFORMATION
			   | GROUP_SECURITY_INFORMATION
			   | OWNER_SECURITY_INFORMATION,
			   sd_ret, &len);
  if (ret == ERROR_INSUFFICIENT_BUFFER)
    {
      if (!sd_ret.malloc (len))
	set_errno (ENOMEM);
      else
	ret = RegGetKeySecurity ((HKEY) handle,
				 DACL_SECURITY_INFORMATION
				 | GROUP_SECURITY_INFORMATION
				 | OWNER_SECURITY_INFORMATION,
				 sd_ret, &len);
    }
  if (ret != ERROR_SUCCESS)
    {
      __seterrno ();
      return -1;
    }
  return 0;
}

int
get_nt_object_security (HANDLE handle, SE_OBJECT_TYPE object_type,
			security_descriptor &sd_ret)
{
  NTSTATUS ret;
  ULONG len = 0;

  /* Do not try to use GetSecurityInfo (again), unless we drop NT4 support.
     GetSecurityInfo returns the wrong user information when running in
     a user session using a token created with NtCreateToken under NT4.
     Works fine in 2K and above, but that doesn't help a lot. */

  /* Unfortunately, NtQuerySecurityObject doesn't work on predefined registry
     keys like HKEY_LOCAL_MACHINE.  It fails with "Invalid Handle".  So we
     have to retreat to the Win32 registry functions for registry keys.
     What bugs me is that RegGetKeySecurity is obviously just a wrapper
     around NtQuerySecurityObject, but there seems to be no function to
     convert pseudo HKEY values to real handles. */
  if (object_type == SE_REGISTRY_KEY)
    return get_reg_security (handle, sd_ret);

  ret = NtQuerySecurityObject (handle,
			       DACL_SECURITY_INFORMATION
			       | GROUP_SECURITY_INFORMATION
			       | OWNER_SECURITY_INFORMATION,
			       sd_ret, len, &len);
  if (ret == STATUS_BUFFER_TOO_SMALL)
    {
      if (!sd_ret.malloc (len))
	set_errno (ENOMEM);
      else
	ret = NtQuerySecurityObject (handle,
				     DACL_SECURITY_INFORMATION
				     | GROUP_SECURITY_INFORMATION
				     | OWNER_SECURITY_INFORMATION,
				     sd_ret, len, &len);
    }
  if (ret != STATUS_SUCCESS)
    {
      __seterrno_from_nt_status (ret);
      return -1;
    }
  return 0;
}

int
get_object_attribute (HANDLE handle, SE_OBJECT_TYPE object_type,
		      mode_t *attribute, __uid32_t *uidret, __gid32_t *gidret)
{
  if (allow_ntsec)
    {
      security_descriptor sd;
      PSECURITY_DESCRIPTOR psd = NULL;

      if (get_nt_object_security (handle, object_type, sd))
	{
	  if (object_type == SE_FILE_OBJECT)
	    return -1;
	}
      else
	psd = sd;
      get_info_from_sd (psd, attribute, uidret, gidret);
      return 0;
    }
  /* The entries are already set to default values */
  return -1;
}

int
get_file_attribute (int use_ntsec, HANDLE handle, const char *file,
		    mode_t *attribute, __uid32_t *uidret, __gid32_t *gidret)
{
  syscall_printf ("file: %s", file);

  if (use_ntsec && allow_ntsec)
    {
      security_descriptor sd;

      if (!handle || get_nt_object_security (handle, SE_FILE_OBJECT, sd))
	read_sd (file, sd);
      get_info_from_sd (sd, attribute, uidret, gidret);
      return 0;
    }

  if (uidret)
    *uidret = myself->uid;
  if (gidret)
    *gidret = myself->gid;

  return -1;
}

bool
add_access_allowed_ace (PACL acl, int offset, DWORD attributes,
			PSID sid, size_t &len_add, DWORD inherit)
{
  if (!AddAccessAllowedAce (acl, ACL_REVISION, attributes, sid))
    {
      __seterrno ();
      return false;
    }
  ACCESS_ALLOWED_ACE *ace;
  if (inherit && GetAce (acl, offset, (PVOID *) &ace))
    ace->Header.AceFlags |= inherit;
  len_add += sizeof (ACCESS_ALLOWED_ACE) - sizeof (DWORD) + GetLengthSid (sid);
  return true;
}

bool
add_access_denied_ace (PACL acl, int offset, DWORD attributes,
		       PSID sid, size_t &len_add, DWORD inherit)
{
  if (!AddAccessDeniedAce (acl, ACL_REVISION, attributes, sid))
    {
      __seterrno ();
      return false;
    }
  ACCESS_DENIED_ACE *ace;
  if (inherit && GetAce (acl, offset, (PVOID *) &ace))
    ace->Header.AceFlags |= inherit;
  len_add += sizeof (ACCESS_DENIED_ACE) - sizeof (DWORD) + GetLengthSid (sid);
  return true;
}

static PSECURITY_DESCRIPTOR
alloc_sd (__uid32_t uid, __gid32_t gid, int attribute,
	  security_descriptor &sd_ret)
{
  BOOL dummy;

  debug_printf("uid %d, gid %d, attribute %x", uid, gid, attribute);

  /* Get owner and group from current security descriptor. */
  PSID cur_owner_sid = NULL;
  PSID cur_group_sid = NULL;
  if (!GetSecurityDescriptorOwner (sd_ret, &cur_owner_sid, &dummy))
    debug_printf ("GetSecurityDescriptorOwner %E");
  if (!GetSecurityDescriptorGroup (sd_ret, &cur_group_sid, &dummy))
    debug_printf ("GetSecurityDescriptorGroup %E");

  /* Get SID of owner. */
  cygsid owner_sid;
  /* Check for current user first */
  if (uid == myself->uid)
    owner_sid = cygheap->user.sid ();
  else if (uid == ILLEGAL_UID)
    owner_sid = cur_owner_sid;
  else if (!owner_sid.getfrompw (internal_getpwuid (uid)))
    {
      set_errno (EINVAL);
      return NULL;
    }
  owner_sid.debug_print ("alloc_sd: owner SID =");

  /* Get SID of new group. */
  cygsid group_sid;
  /* Check for current user first */
  if (gid == myself->gid)
    group_sid = cygheap->user.groups.pgsid;
  else if (gid == ILLEGAL_GID)
    group_sid = cur_group_sid;
  else if (!group_sid.getfromgr (internal_getgrgid (gid)))
    {
      set_errno (EINVAL);
      return NULL;
    }
  group_sid.debug_print ("alloc_sd: group SID =");

  /* Initialize local security descriptor. */
  SECURITY_DESCRIPTOR sd;
  if (!InitializeSecurityDescriptor (&sd, SECURITY_DESCRIPTOR_REVISION))
    {
      __seterrno ();
      return NULL;
    }

  /* We set the SE_DACL_PROTECTED flag here to prevent the DACL from being
   * modified by inheritable ACEs.  This flag is available since Win2K.  */
  if (wincap.has_dacl_protect ())
    sd.Control |= SE_DACL_PROTECTED;

  /* Create owner for local security descriptor. */
  if (!SetSecurityDescriptorOwner (&sd, owner_sid, FALSE))
    {
      __seterrno ();
      return NULL;
    }

  /* Create group for local security descriptor. */
  if (!SetSecurityDescriptorGroup (&sd, group_sid, FALSE))
    {
      __seterrno ();
      return NULL;
    }

  /* Initialize local access control list. */
  PACL acl = (PACL) alloca (3072);
  if (!InitializeAcl (acl, 3072, ACL_REVISION))
    {
      __seterrno ();
      return NULL;
    }

  /* From here fill ACL. */
  size_t acl_len = sizeof (ACL);
  int ace_off = 0;

  /* Construct allow attribute for owner. */
  DWORD owner_allow = STANDARD_RIGHTS_ALL
		      | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
  if (attribute & S_IRUSR)
    owner_allow |= FILE_GENERIC_READ;
  if (attribute & S_IWUSR)
    owner_allow |= FILE_GENERIC_WRITE;
  if (attribute & S_IXUSR)
    owner_allow |= FILE_GENERIC_EXECUTE;
  if (S_ISDIR (attribute)
      && (attribute & (S_IWUSR | S_IXUSR)) == (S_IWUSR | S_IXUSR))
    owner_allow |= FILE_DELETE_CHILD;

  /* Construct allow attribute for group. */
  DWORD group_allow = STANDARD_RIGHTS_READ
		      | FILE_READ_ATTRIBUTES | FILE_READ_EA;
  if (attribute & S_IRGRP)
    group_allow |= FILE_GENERIC_READ;
  if (attribute & S_IWGRP)
    group_allow |= STANDARD_RIGHTS_WRITE | FILE_GENERIC_WRITE;
  if (attribute & S_IXGRP)
    group_allow |= FILE_GENERIC_EXECUTE;
  if (S_ISDIR (attribute)
      && (attribute & (S_IWGRP | S_IXGRP)) == (S_IWGRP | S_IXGRP)
      && !(attribute & S_ISVTX))
    group_allow |= FILE_DELETE_CHILD;

  /* Construct allow attribute for everyone. */
  DWORD other_allow = STANDARD_RIGHTS_READ
		      | FILE_READ_ATTRIBUTES | FILE_READ_EA;
  if (attribute & S_IROTH)
    other_allow |= FILE_GENERIC_READ;
  if (attribute & S_IWOTH)
    other_allow |= STANDARD_RIGHTS_WRITE | FILE_GENERIC_WRITE;
  if (attribute & S_IXOTH)
    other_allow |= FILE_GENERIC_EXECUTE;
  if (S_ISDIR (attribute)
      && (attribute & (S_IWOTH | S_IXOTH)) == (S_IWOTH | S_IXOTH)
      && !(attribute & S_ISVTX))
    other_allow |= FILE_DELETE_CHILD;

  /* Construct SUID, SGID and VTX bits in NULL ACE. */
  DWORD null_allow = 0L;
  if (attribute & (S_ISUID | S_ISGID | S_ISVTX))
    {
      if (attribute & S_ISUID)
	null_allow |= FILE_APPEND_DATA;
      if (attribute & S_ISGID)
	null_allow |= FILE_WRITE_DATA;
      if (attribute & S_ISVTX)
	null_allow |= FILE_READ_DATA;
    }

  /* Add owner and group permissions if SIDs are equal
     and construct deny attributes for group and owner. */
  bool isownergroup;
  if ((isownergroup = (owner_sid == group_sid)))
    owner_allow |= group_allow;

  DWORD owner_deny = ~owner_allow & (group_allow | other_allow);
  owner_deny &= ~(STANDARD_RIGHTS_READ
		  | FILE_READ_ATTRIBUTES | FILE_READ_EA
		  | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA);

  DWORD group_deny = ~group_allow & other_allow;
  group_deny &= ~(STANDARD_RIGHTS_READ
		  | FILE_READ_ATTRIBUTES | FILE_READ_EA);

  /* Set deny ACE for owner. */
  if (owner_deny
      && !add_access_denied_ace (acl, ace_off++, owner_deny,
				 owner_sid, acl_len, NO_INHERITANCE))
    return NULL;
  /* Set deny ACE for group here to respect the canonical order,
     if this does not impact owner */
  if (group_deny && !(group_deny & owner_allow) && !isownergroup
      && !add_access_denied_ace (acl, ace_off++, group_deny,
				 group_sid, acl_len, NO_INHERITANCE))
    return NULL;
  /* Set allow ACE for owner. */
  if (!add_access_allowed_ace (acl, ace_off++, owner_allow,
			       owner_sid, acl_len, NO_INHERITANCE))
    return NULL;
  /* Set deny ACE for group, if still needed. */
  if (group_deny & owner_allow && !isownergroup
      && !add_access_denied_ace (acl, ace_off++, group_deny,
				 group_sid, acl_len, NO_INHERITANCE))
    return NULL;
  /* Set allow ACE for group. */
  if (!isownergroup
      && !add_access_allowed_ace (acl, ace_off++, group_allow,
				  group_sid, acl_len, NO_INHERITANCE))
    return NULL;

  /* Set allow ACE for everyone. */
  if (!add_access_allowed_ace (acl, ace_off++, other_allow,
			       well_known_world_sid, acl_len, NO_INHERITANCE))
    return NULL;
  /* Set null ACE for special bits. */
  if (null_allow
      && !add_access_allowed_ace (acl, ace_off++, null_allow,
				  well_known_null_sid, acl_len, NO_INHERITANCE))
    return NULL;

  /* Fill ACL with unrelated ACEs from current security descriptor. */
  PACL oacl;
  BOOL acl_exists = FALSE;
  ACCESS_ALLOWED_ACE *ace;
  if (GetSecurityDescriptorDacl (sd_ret, &acl_exists, &oacl, &dummy)
      && acl_exists && oacl)
    for (DWORD i = 0; i < oacl->AceCount; ++i)
      if (GetAce (oacl, i, (PVOID *) &ace))
	{
	  cygpsid ace_sid ((PSID) &ace->SidStart);

	  /* Check for related ACEs. */
	  if (ace_sid == well_known_null_sid)
	    continue;
	  if ((ace_sid == cur_owner_sid)
	      || (ace_sid == owner_sid)
	      || (ace_sid == cur_group_sid)
	      || (ace_sid == group_sid)
	      || (ace_sid == well_known_world_sid))
	    {
	      if (ace->Header.AceFlags & SUB_CONTAINERS_AND_OBJECTS_INHERIT)
		ace->Header.AceFlags |= INHERIT_ONLY;
	      else
		continue;
	    }
	  /*
	   * Add unrelated ACCESS_DENIED_ACE to the beginning but
	   * behind the owner_deny, ACCESS_ALLOWED_ACE to the end.
	   * FIXME: this would break the order of the inherit_only ACEs
	   */
	  if (!AddAce (acl, ACL_REVISION,
		       ace->Header.AceType == ACCESS_DENIED_ACE_TYPE?
		       (owner_deny ? 1 : 0) : MAXDWORD,
		       (LPVOID) ace, ace->Header.AceSize))
	    {
	      __seterrno ();
	      return NULL;
	    }
	  acl_len += ace->Header.AceSize;
	}

  /* Construct appropriate inherit attribute for new directories */
  if (S_ISDIR (attribute) && !acl_exists)
    {
      const DWORD inherit = SUB_CONTAINERS_AND_OBJECTS_INHERIT | INHERIT_ONLY;

#if 0 /* FIXME: Not done currently as this breaks the canonical order */
      /* Set deny ACE for owner. */
      if (owner_deny
	  && !add_access_denied_ace (acl, ace_off++, owner_deny,
				     well_known_creator_owner_sid, acl_len, inherit))
	return NULL;
      /* Set deny ACE for group here to respect the canonical order,
	 if this does not impact owner */
      if (group_deny && !(group_deny & owner_allow)
	  && !add_access_denied_ace (acl, ace_off++, group_deny,
				     well_known_creator_group_sid, acl_len, inherit))
	return NULL;
#endif
      /* Set allow ACE for owner. */
      if (!add_access_allowed_ace (acl, ace_off++, owner_allow,
				   well_known_creator_owner_sid, acl_len, inherit))
	return NULL;
#if 0 /* FIXME: Not done currently as this breaks the canonical order and
	 won't be preserved on chown and chmod */
      /* Set deny ACE for group, conflicting with owner_allow. */
      if (group_deny & owner_allow
	  && !add_access_denied_ace (acl, ace_off++, group_deny,
				     well_known_creator_group_sid, acl_len, inherit))
	return NULL;
#endif
      /* Set allow ACE for group. */
      if (!add_access_allowed_ace (acl, ace_off++, group_allow,
				   well_known_creator_group_sid, acl_len, inherit))
	return NULL;
      /* Set allow ACE for everyone. */
      if (!add_access_allowed_ace (acl, ace_off++, other_allow,
				   well_known_world_sid, acl_len, inherit))
	return NULL;
    }

  /* Set AclSize to computed value. */
  acl->AclSize = acl_len;
  debug_printf ("ACL-Size: %d", acl_len);

  /* Create DACL for local security descriptor. */
  if (!SetSecurityDescriptorDacl (&sd, TRUE, acl, FALSE))
    {
      __seterrno ();
      return NULL;
    }

  /* Make self relative security descriptor. */
  DWORD sd_size = 0;
  MakeSelfRelativeSD (&sd, sd_ret, &sd_size);
  if (sd_size <= 0)
    {
      __seterrno ();
      return NULL;
    }
  if (!sd_ret.malloc (sd_size))
    {
      set_errno (ENOMEM);
      return NULL;
    }
  if (!MakeSelfRelativeSD (&sd, sd_ret, &sd_size))
    {
      __seterrno ();
      return NULL;
    }
  debug_printf ("Created SD-Size: %u", sd_ret.size ());

  return sd_ret;
}

void
set_security_attribute (int attribute, PSECURITY_ATTRIBUTES psa,
			security_descriptor &sd)
{
  psa->lpSecurityDescriptor = sd.malloc (SECURITY_DESCRIPTOR_MIN_LENGTH);
  InitializeSecurityDescriptor ((PSECURITY_DESCRIPTOR)psa->lpSecurityDescriptor,
				SECURITY_DESCRIPTOR_REVISION);
  psa->lpSecurityDescriptor = alloc_sd (geteuid32 (), getegid32 (),
					attribute, sd);
}

int
set_file_attribute (bool use_ntsec, HANDLE handle, const char *file,
		    __uid32_t uid, __gid32_t gid, int attribute)
{
  int ret = -1;

  if (use_ntsec && allow_ntsec)
    {
      security_descriptor sd;

      if (((handle && !get_nt_object_security (handle, SE_FILE_OBJECT, sd))
	   || read_sd (file, sd) > 0)
	  && alloc_sd (uid, gid, attribute, sd))
	ret = write_sd (handle, file, sd);
    }
  else
    ret = 0;
  syscall_printf ("%d = set_file_attribute (%s, %d, %d, %p)",
		  ret, file, uid, gid, attribute);
  return ret;
}

int
set_file_attribute (bool use_ntsec, HANDLE handle, const char *file,
		    int attribute)
{
  return set_file_attribute (use_ntsec, handle, file,
			     myself->uid, myself->gid, attribute);
}

static int
check_access (security_descriptor &sd, GENERIC_MAPPING &mapping,
	      DWORD desired, int flags)
{
  int ret = -1;
  BOOL status;
  DWORD granted;
  DWORD plen = sizeof (PRIVILEGE_SET) + 3 * sizeof (LUID_AND_ATTRIBUTES);
  PPRIVILEGE_SET pset = (PPRIVILEGE_SET) alloca (plen);
  HANDLE tok = cygheap->user.issetuid () ? cygheap->user.imp_token ()
					 : hProcImpToken;

  if (!tok && !DuplicateTokenEx (hProcToken, MAXIMUM_ALLOWED, NULL,
				 SecurityImpersonation, TokenImpersonation,
				 &hProcImpToken))
#ifdef DEBUGGING
	system_printf ("DuplicateTokenEx failed, %E");
#else
	syscall_printf ("DuplicateTokenEx failed, %E");
#endif
  else
    tok = hProcImpToken;

  if (!AccessCheck (sd, tok, desired, &mapping, pset, &plen, &granted, &status))
    __seterrno ();
  else if (!status)
    {
      /* CV, 2006-10-16: Now, that's really weird.  Imagine a user who has no
	 standard access to a file, but who has backup and restore privileges
	 and these privileges are enabled in the access token.  One would
	 expect that the AccessCheck function takes this into consideration
	 when returning the access status.  Otherwise, why bother with the
	 pset parameter, right?
	 But not so.  AccessCheck actually returns a status of "false" here,
	 even though opening a file with backup resp.  restore intent
	 naturally succeeds for this user.  This definitely spoils the results
	 of access(2) for administrative users or the SYSTEM account.  So, in
	 case the access check fails, another check against the user's
	 backup/restore privileges has to be made.  Sigh. */
      int granted_flags = 0;
      if (flags & R_OK)
	{
	  pset->PrivilegeCount = 1;
	  pset->Control = 0;
	  pset->Privilege[0].Luid.HighPart = 0L;
	  pset->Privilege[0].Luid.LowPart = SE_BACKUP_PRIVILEGE;
	  pset->Privilege[0].Attributes = 0;
	  if (PrivilegeCheck (tok, pset, &status) && status)
	    granted_flags |= R_OK;
	}
      if (flags & W_OK)
	{
	  pset->PrivilegeCount = 1;
	  pset->Control = 0;
	  pset->Privilege[0].Luid.HighPart = 0L;
	  pset->Privilege[0].Luid.LowPart = SE_RESTORE_PRIVILEGE;
	  pset->Privilege[0].Attributes = 0;
	  if (PrivilegeCheck (tok, pset, &status) && status)
	    granted_flags |= W_OK;
	}
      if (granted_flags == flags)
	ret = 0;
      else
	set_errno (EACCES);
    }
  else
    ret = 0;
  return ret;
}

int
check_file_access (const char *fn, int flags)
{
  security_descriptor sd;
  int ret = -1;
  static GENERIC_MAPPING NO_COPY mapping = { FILE_GENERIC_READ,
					     FILE_GENERIC_WRITE,
					     FILE_GENERIC_EXECUTE,
					     FILE_ALL_ACCESS };
  DWORD desired = 0;
  if (flags & R_OK)
    desired |= FILE_READ_DATA;
  if (flags & W_OK)
    desired |= FILE_WRITE_DATA;
  if (flags & X_OK)
    desired |= FILE_EXECUTE;
  if (read_sd (fn, sd) > 0)
    ret = check_access (sd, mapping, desired, flags);
  debug_printf ("flags %x, ret %d", flags, ret);
  return ret;
}

int
check_registry_access (HANDLE hdl, int flags)
{
  security_descriptor sd;
  int ret = -1;
  static GENERIC_MAPPING NO_COPY mapping = { KEY_READ,
					     KEY_WRITE,
					     KEY_EXECUTE,
					     KEY_ALL_ACCESS };
  DWORD desired = 0;
  if (flags & R_OK)
    desired |= KEY_ENUMERATE_SUB_KEYS;
  if (flags & W_OK)
    desired |= KEY_SET_VALUE;
  if (flags & X_OK)
    desired |= KEY_QUERY_VALUE;
  if (!get_nt_object_security (hdl, SE_REGISTRY_KEY, sd))
    ret = check_access (sd, mapping, desired, flags);
  /* As long as we can't write the registry... */
  if (flags & W_OK)
    {
      set_errno (EROFS);
      ret = -1;
    }
  debug_printf ("flags %x, ret %d", flags, ret);
  return ret;
}
