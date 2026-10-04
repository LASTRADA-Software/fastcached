# SPDX-License-Identifier: Apache-2.0
#
# The service table in packaging/windows/service-actions.xml, asserted element by
# element.
#
# The MSI is the one artefact here whose behaviour only a Windows runner can
# exercise, minutes into the slowest job in the matrix, and a condition that lost a
# clause still builds a package. Two of the clauses are guards whose absence would
# do damage rather than fail: the leftover removal deletes a SERVICE, so it must be
# keyed on an upgrade, on the feature being gone, and on the registration pointing
# into this install root (a service registered by hand from another directory is
# not the installer's to delete). This check pins those clauses in the source,
# where a change to them is a red test on every platform.
#
# What it asserts:
#   1. every row of the table below: the element opening with `opener` exists
#      exactly once, and the text up to its `terminator` contains `required`
#      (entities decoded), or, for a `required` starting with `!`, does NOT;
#   2. every Condition attribute that reads one of the 0-then-1 flags (which
#      features are left installed, whether a registration is ours) compares it
#      with "1". Those properties are written 0 and then 1, so a bare truth test
#      reads "0" as true.
#
# The After= rows also pin the ONE sequence: validate, register, remember, start,
# and only then delete. The node's arguments are CHECKED first, directly after the
# files (FastCacheNodeCheckArguments, Return="check"), so a fleet pin its parser
# refuses fails the transaction with nothing changed (B3-1, and B4-3: the leftover
# deletions once ran before it, and nothing restores a deleted service, so they are
# LAST now); the registration and the waiting start are Return="check" (ci-fix2);
# fastcached stopped for the node is waited for until its PROCESS exits, and an
# upgrade waits for the old product's service processes before its files (B4-2);
# and every step a later failure would orphan has its ROLLBACK twin scheduled just
# before it -- a registration this transaction created is removed, one it
# re-applied is put back exactly from a copy of its key (B4-1, B4-4), a service it
# stopped while running is started again, and each remembered value is written
# back or deleted as it was found -- so a failed step leaves nothing behind.
#
# What it does NOT see: whether Windows Installer evaluates a condition the way it
# reads, and anything about sequencing beyond the After= values pinned below. The
# `Package (Windows .msi)` job is the only thing that runs the package. This fails
# CLOSED on a row whose opener is missing or repeated, and OPEN on a clause that is
# present but wrong in a way no row names.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-wix-service-table.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(_file "${FASTCACHED_SOURCE_DIR}/packaging/windows/service-actions.xml")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "check-wix-service-table: ${_file} does not exist")
endif()

# One row per line: opener | terminator | required. ONE bracket argument walked
# line by line with FIND and SUBSTRING, never a CMake list: a row carries
# `[INSTALL_ROOT]`, and a square bracket or a `;` in a list element merges or
# drops elements in silence. A blank line is a separator and is skipped.
set(_table [=[
<Property Id="FASTCACHE_NODE_IMAGEPATH"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCacheCompileNode"
<Property Id="FASTCACHE_NODE_IMAGEPATH"|</Property>|Name="ImagePath"
<Property Id="FASTCACHE_NODE_IMAGEPATH"|</Property>|Root="HKLM"
<Property Id="FASTCACHED_IMAGEPATH"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCached"
<Property Id="FASTCACHED_IMAGEPATH"|</Property>|Name="ImagePath"
<Property Id="FASTCACHED_IMAGEPATH"|</Property>|Root="HKLM"

<CustomAction Id="FastCacheNodeDeleteLeftover"|/>|sc.exe" delete FastCacheCompileNode"
<CustomAction Id="FastCacheNodeDeleteLeftover"|/>|Execute="deferred"
<CustomAction Id="FastCacheNodeDeleteLeftover"|/>|Impersonate="no"
<CustomAction Id="FastCacheNodeDeleteLeftover"|/>|Return="ignore"
<CustomAction Id="FastCachedDeleteLeftover"|/>|sc.exe" delete FastCached"
<CustomAction Id="FastCachedDeleteLeftover"|/>|Execute="deferred"
<CustomAction Id="FastCachedDeleteLeftover"|/>|Impersonate="no"
<CustomAction Id="FastCachedDeleteLeftover"|/>|Return="ignore"

<Custom Action="FastCacheNodeDeleteLeftover"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCacheNodeDeleteLeftover"|/>|NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCacheNodeDeleteLeftover"|/>|NOT (FASTCACHE_NODE_SELECTED = "1")
<Custom Action="FastCacheNodeDeleteLeftover"|/>|FASTCACHE_NODE_REGISTERED_HERE = "1"
<Custom Action="FastCacheNodeDeleteLeftover"|/>|!~><
<Custom Action="FastCacheNodeDeleteLeftover"|/>|After="FastCacheDiscardRollbackState"
<Custom Action="FastCachedDeleteLeftover"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCachedDeleteLeftover"|/>|NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCachedDeleteLeftover"|/>|NOT (FASTCACHED_SELECTED = "1")
<Custom Action="FastCachedDeleteLeftover"|/>|FASTCACHED_REGISTERED_HERE = "1"
<Custom Action="FastCachedDeleteLeftover"|/>|!~><
<Custom Action="FastCachedDeleteLeftover"|/>|After="FastCacheNodeDeleteLeftoverFirewall"
<CustomAction Id="FastCacheNodeDeleteLeftoverFirewall"|/>|Remove-NetFirewallRule -Group 'fastcached: FastCacheCompileNode'
<CustomAction Id="FastCacheNodeDeleteLeftoverFirewall"|/>|Execute="deferred"
<CustomAction Id="FastCacheNodeDeleteLeftoverFirewall"|/>|Impersonate="no"
<CustomAction Id="FastCacheNodeDeleteLeftoverFirewall"|/>|Return="ignore"
<CustomAction Id="FastCachedDeleteLeftoverFirewall"|/>|Remove-NetFirewallRule -Group 'fastcached: FastCached'
<CustomAction Id="FastCachedDeleteLeftoverFirewall"|/>|Execute="deferred"
<CustomAction Id="FastCachedDeleteLeftoverFirewall"|/>|Impersonate="no"
<CustomAction Id="FastCachedDeleteLeftoverFirewall"|/>|Return="ignore"
<Custom Action="FastCacheNodeDeleteLeftoverFirewall"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCacheNodeDeleteLeftoverFirewall"|/>|NOT (FASTCACHE_NODE_SELECTED = "1")
<Custom Action="FastCacheNodeDeleteLeftoverFirewall"|/>|FASTCACHE_NODE_REGISTERED_HERE = "1"
<Custom Action="FastCacheNodeDeleteLeftoverFirewall"|/>|After="FastCacheNodeDeleteLeftover"
<Custom Action="FastCachedDeleteLeftoverFirewall"|/>|WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE
<Custom Action="FastCachedDeleteLeftoverFirewall"|/>|NOT (FASTCACHED_SELECTED = "1")
<Custom Action="FastCachedDeleteLeftoverFirewall"|/>|FASTCACHED_REGISTERED_HERE = "1"
<Custom Action="FastCachedDeleteLeftoverFirewall"|/>|After="FastCachedDeleteLeftover"
<Custom Action="FastCachedSeedConfig"|/>|After="FastCacheNodeRestartAfterStop"
<CustomAction Id="FastCacheNodeInstallService"|/>|!FastCacheNodeAdvertiseArgument
<CustomAction Id="FastCacheNodeCheckArguments"|/>|!FastCacheNodeAdvertiseArgument
<CustomAction Id="FastCacheNodeRestoreRegistration"|/>|!FastCacheNodeAdvertise
<CustomAction Id="FastCacheForgetNodeAdvertise"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v NodeAdvertise /f /reg:64"
<CustomAction Id="FastCacheForgetNodeAdvertise"|/>|Execute="deferred"
<CustomAction Id="FastCacheForgetNodeAdvertise"|/>|Impersonate="no"
<CustomAction Id="FastCacheForgetNodeAdvertise"|/>|Return="ignore"
<CPackWiXFragment Id="#PRODUCT">|</CPackWiXFragment>|!FASTCACHE_NODE_ADVERTISE
<Custom Action="FastCacheForgetNodeAdvertise"|/>|Condition="FASTCACHED_SELECTED = "1" OR FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheForgetNodeAdvertise"|/>|After="FastCacheRememberFleetId"

<CustomAction Id="FastCacheNodeInstallService"|/>|!--discovery-reply-port=
<CustomAction Id="FastCacheNodeInstallService"|/>|!--scheduler
<CustomAction Id="FastCacheNodeInstallService"|/>|Return="check"
<CustomAction Id="FastCacheNodeStartService"|/>|Return="check"
<CustomAction Id="FastCacheNodeStartService"|/>|/c [System64Folder]net.exe start FastCacheCompileNode && [System64Folder]sc.exe query FastCacheCompileNode | [System64Folder]find.exe "RUNNING""
<CustomAction Id="FastCacheNodeStartService"|/>|!ping.exe
<CustomAction Id="FastCacheNodeStartService"|/>|!||
<CustomAction Id="FastCacheNodeStartService"|/>|!sc.exe" start
<CustomAction Id="FastCachedInstallService"|/>|!DiscoveryReply
<Property Id="FASTCACHE_DISCOVERY_REPLY_PORT"|/>|Value="6682"
<CustomAction Id="FastCachedInstallService"|/>|--service-start=[FASTCACHED_START_MODE] [FastCacheFirewallAllowArgument]"
<CustomAction Id="FastCachedInstallService"|/>|!--firewall-allow=
<CustomAction Id="FastCacheNodeInstallService"|/>|!--firewall-allow=
<CustomAction Id="FastCacheNodeInstallService"|/>|!--cluster-dir
<CustomAction Id="FastCacheNodeInstallService"|/>|!--advertise=
<CustomAction Id="FastCacheNodeInstallService"|/>|!--fleet-seed=
<CustomAction Id="FastCacheNodeInstallService"|/>|!--fleet-id=
<Custom Action="FastCacheNodeInstallService"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheNodeInstallService"|/>|!FASTCACHE_NODE_SCHEDULER
<Custom Action="FastCacheNodeInstallService"|/>|!FASTCACHE_NODE_ADVERTISE
<Custom Action="FastCacheNodeInstallService"|/>|!FASTCACHE_FLEET_SEED
<Custom Action="FastCacheNodeInstallService"|/>|!FASTCACHE_FLEET_ID
<SetProperty Action="SetFastCacheFirewallAllowArgument"|/>|Id="FastCacheFirewallAllowArgument"
<SetProperty Action="SetFastCacheFirewallAllowArgument"|/>|Value="--firewall-allow=[FASTCACHE_FIREWALL_ALLOW]"
<SetProperty Action="SetFastCacheFirewallAllowArgument"|/>|Condition="FASTCACHE_FIREWALL_ALLOW"
<SetProperty Action="SetFastCacheNodeDiscoveryReplyArgument"|/>|Id="FastCacheNodeDiscoveryReplyArgument"
<SetProperty Action="SetFastCacheNodeDiscoveryReplyArgument"|/>|Value="--discovery-reply-port=[FASTCACHE_DISCOVERY_REPLY_PORT]"
<SetProperty Action="SetFastCacheNodeDiscoveryReplyArgument"|/>|Condition="FASTCACHE_DISCOVERY_REPLY_PORT"
<SetProperty Action="SetFastCacheNodeDiscoveryReplyArgument"|/>|After="SetFastCacheFirewallAllowArgument"
<SetProperty Action="SetFastCacheFleetSeedArgument"|/>|Id="FastCacheFleetSeedArgument"
<SetProperty Action="SetFastCacheFleetSeedArgument"|/>|Value="--fleet-seed=[FASTCACHE_FLEET_SEED]"
<SetProperty Action="SetFastCacheFleetSeedArgument"|/>|Condition="FASTCACHE_FLEET_SEED"
<SetProperty Action="SetFastCacheFleetSeedArgument"|/>|After="SetFastCacheNodeDiscoveryReplyArgument"
<SetProperty Action="SetFastCacheFleetIdArgument"|/>|Id="FastCacheFleetIdArgument"
<SetProperty Action="SetFastCacheFleetIdArgument"|/>|Value="--fleet-id=[FASTCACHE_FLEET_ID]"
<SetProperty Action="SetFastCacheFleetIdArgument"|/>|Condition="FASTCACHE_FLEET_ID"
<SetProperty Action="SetFastCacheFleetIdArgument"|/>|After="SetFastCacheFleetSeedArgument"

<Property Id="FASTCACHE_FIREWALL_ALLOW"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FIREWALL_ALLOW"|</Property>|Name="FirewallAllow"
<Property Id="FASTCACHE_FIREWALL_ALLOW"|</Property>|Root="HKLM"
<Property Id="FASTCACHE_FLEET_SEED"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FLEET_SEED"|</Property>|Name="FleetSeed"
<Property Id="FASTCACHE_FLEET_SEED"|</Property>|Root="HKLM"
<Property Id="FASTCACHE_FLEET_ID"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FLEET_ID"|</Property>|Name="FleetId"
<Property Id="FASTCACHE_FLEET_ID"|</Property>|Root="HKLM"
<SetProperty Action="SaveTypedFastCacheFirewallAllow"|/>|Value="[FASTCACHE_FIREWALL_ALLOW]"
<SetProperty Action="SaveTypedFastCacheFirewallAllow"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedFastCacheFirewallAllow"|/>|Condition="FASTCACHE_FIREWALL_ALLOW"
<SetProperty Action="RestoreTypedFastCacheFirewallAllow"|/>|Id="FASTCACHE_FIREWALL_ALLOW"
<SetProperty Action="RestoreTypedFastCacheFirewallAllow"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedFastCacheFirewallAllow"|/>|Condition="FastCacheFirewallAllowTyped"
<SetProperty Action="SaveTypedFastCacheFleetSeed"|/>|Value="[FASTCACHE_FLEET_SEED]"
<SetProperty Action="SaveTypedFastCacheFleetSeed"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedFastCacheFleetSeed"|/>|Condition="FASTCACHE_FLEET_SEED"
<SetProperty Action="RestoreTypedFastCacheFleetSeed"|/>|Id="FASTCACHE_FLEET_SEED"
<SetProperty Action="RestoreTypedFastCacheFleetSeed"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedFastCacheFleetSeed"|/>|Condition="FastCacheFleetSeedTyped"
<SetProperty Action="SaveTypedFastCacheFleetId"|/>|Value="[FASTCACHE_FLEET_ID]"
<SetProperty Action="SaveTypedFastCacheFleetId"|/>|Before="AppSearch" Sequence="both"
<SetProperty Action="SaveTypedFastCacheFleetId"|/>|Condition="FASTCACHE_FLEET_ID"
<SetProperty Action="RestoreTypedFastCacheFleetId"|/>|Id="FASTCACHE_FLEET_ID"
<SetProperty Action="RestoreTypedFastCacheFleetId"|/>|After="AppSearch" Sequence="both"
<SetProperty Action="RestoreTypedFastCacheFleetId"|/>|Condition="FastCacheFleetIdTyped"
<Component Id="FastCacheRememberedProperties"|</Component>|Guid="87BB5EEE-8B9E-4401-9367-C86DB5061258"
<Component Id="FastCacheRememberedProperties"|</Component>|Action="removeOnUninstall"
<ComponentRef Id="FastCacheRememberedProperties"|/>|ComponentRef
<CustomAction Id="FastCacheRememberFirewallAllow"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /t REG_SZ /d "[FASTCACHE_FIREWALL_ALLOW]" /f /reg:64"
<CustomAction Id="FastCacheRememberFirewallAllow"|/>|Execute="deferred"
<CustomAction Id="FastCacheRememberFirewallAllow"|/>|Impersonate="no"
<CustomAction Id="FastCacheRememberFirewallAllow"|/>|Return="check"
<CustomAction Id="FastCacheRememberFleetSeed"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /t REG_SZ /d "[FASTCACHE_FLEET_SEED]" /f /reg:64"
<CustomAction Id="FastCacheRememberFleetSeed"|/>|Execute="deferred"
<CustomAction Id="FastCacheRememberFleetSeed"|/>|Impersonate="no"
<CustomAction Id="FastCacheRememberFleetSeed"|/>|Return="check"
<CustomAction Id="FastCacheRememberFleetId"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetId /t REG_SZ /d "[FASTCACHE_FLEET_ID]" /f /reg:64"
<CustomAction Id="FastCacheRememberFleetId"|/>|Execute="deferred"
<CustomAction Id="FastCacheRememberFleetId"|/>|Impersonate="no"
<CustomAction Id="FastCacheRememberFleetId"|/>|Return="check"
<CustomAction Id="FastCacheNodeCheckArguments"|/>|Execute="deferred"
<CustomAction Id="FastCacheNodeCheckArguments"|/>|Impersonate="no"
<CustomAction Id="FastCacheNodeCheckArguments"|/>|Return="check"
<Custom Action="FastCacheNodeCheckArguments"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheNodeCheckArguments"|/>|After="InstallFiles"
<Custom Action="FastCacheRememberFirewallAllow"|/>|Condition="FASTCACHED_SELECTED = "1" OR FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheRememberFleetSeed"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheRememberFleetSeed"|/>|!FASTCACHED_SELECTED
<Custom Action="FastCacheRememberFleetId"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""
<Custom Action="FastCacheRememberFleetId"|/>|!FASTCACHED_SELECTED
<Property Id="FASTCACHE_FIREWALL_ALLOW_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FIREWALL_ALLOW_BEFORE"|</Property>|Name="FirewallAllow"
<CustomAction Id="FastCacheRollbackRestoreFirewallAllow"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /t REG_SZ /d "[FASTCACHE_FIREWALL_ALLOW_BEFORE]" /f /reg:64"
<CustomAction Id="FastCacheRollbackRestoreFirewallAllow"|/>|Execute="rollback"
<CustomAction Id="FastCacheRollbackDeleteFirewallAllow"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FirewallAllow /f /reg:64"
<CustomAction Id="FastCacheRollbackDeleteFirewallAllow"|/>|Execute="rollback"
<Custom Action="FastCacheRollbackRestoreFirewallAllow"|/>|After="FastCacheNodeInstallService"
<Custom Action="FastCacheRollbackRestoreFirewallAllow"|/>|Condition="(FASTCACHED_SELECTED = "1" OR FASTCACHE_NODE_SELECTED = "1") AND FASTCACHE_FIREWALL_ALLOW_BEFORE"
<Custom Action="FastCacheRollbackDeleteFirewallAllow"|/>|After="FastCacheRollbackRestoreFirewallAllow"
<Custom Action="FastCacheRollbackDeleteFirewallAllow"|/>|Condition="(FASTCACHED_SELECTED = "1" OR FASTCACHE_NODE_SELECTED = "1") AND NOT FASTCACHE_FIREWALL_ALLOW_BEFORE"
<Custom Action="FastCacheRememberFirewallAllow"|/>|After="FastCacheRollbackDeleteFirewallAllow"
<Property Id="FASTCACHE_FLEET_SEED_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FLEET_SEED_BEFORE"|</Property>|Name="FleetSeed"
<CustomAction Id="FastCacheRollbackRestoreFleetSeed"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /t REG_SZ /d "[FASTCACHE_FLEET_SEED_BEFORE]" /f /reg:64"
<CustomAction Id="FastCacheRollbackRestoreFleetSeed"|/>|Execute="rollback"
<CustomAction Id="FastCacheRollbackDeleteFleetSeed"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FleetSeed /f /reg:64"
<CustomAction Id="FastCacheRollbackDeleteFleetSeed"|/>|Execute="rollback"
<Custom Action="FastCacheRollbackRestoreFleetSeed"|/>|Condition="(FASTCACHE_NODE_SELECTED = "1") AND FASTCACHE_FLEET_SEED_BEFORE"
<Custom Action="FastCacheRollbackDeleteFleetSeed"|/>|After="FastCacheRollbackRestoreFleetSeed"
<Custom Action="FastCacheRollbackDeleteFleetSeed"|/>|Condition="(FASTCACHE_NODE_SELECTED = "1") AND NOT FASTCACHE_FLEET_SEED_BEFORE"
<Custom Action="FastCacheRememberFleetSeed"|/>|After="FastCacheRollbackDeleteFleetSeed"
<Property Id="FASTCACHE_FLEET_ID_BEFORE"|</Property>|Key="SOFTWARE\fastcached\Installer"
<Property Id="FASTCACHE_FLEET_ID_BEFORE"|</Property>|Name="FleetId"
<CustomAction Id="FastCacheRollbackRestoreFleetId"|/>|reg.exe" add HKLM\SOFTWARE\fastcached\Installer /v FleetId /t REG_SZ /d "[FASTCACHE_FLEET_ID_BEFORE]" /f /reg:64"
<CustomAction Id="FastCacheRollbackRestoreFleetId"|/>|Execute="rollback"
<CustomAction Id="FastCacheRollbackDeleteFleetId"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\Installer /v FleetId /f /reg:64"
<CustomAction Id="FastCacheRollbackDeleteFleetId"|/>|Execute="rollback"
<Custom Action="FastCacheRollbackRestoreFleetId"|/>|After="FastCacheRememberFleetSeed"
<Custom Action="FastCacheRollbackRestoreFleetId"|/>|Condition="(FASTCACHE_NODE_SELECTED = "1") AND FASTCACHE_FLEET_ID_BEFORE"
<Custom Action="FastCacheRollbackDeleteFleetId"|/>|After="FastCacheRollbackRestoreFleetId"
<Custom Action="FastCacheRollbackDeleteFleetId"|/>|Condition="(FASTCACHE_NODE_SELECTED = "1") AND NOT FASTCACHE_FLEET_ID_BEFORE"
<Custom Action="FastCacheRememberFleetId"|/>|After="FastCacheRollbackDeleteFleetId"
<CustomAction Id="FastCachedUndoRegistration"|/>|fastcached.exe" --uninstall-service"
<CustomAction Id="FastCachedUndoRegistration"|/>|Execute="rollback"
<Custom Action="FastCachedUndoRegistration"|/>|After="FastCachedRestoreRegistrationExactly"
<Custom Action="FastCachedUndoRegistration"|/>|Condition="FASTCACHED_SELECTED = "1" AND NOT FASTCACHED_IMAGEPATH"
<Custom Action="FastCachedInstallService"|/>|After="FastCachedRestoreRegistration"
<CustomAction Id="FastCacheNodeUndoRegistration"|/>|fastcache-compile-node.exe" --uninstall-service"
<CustomAction Id="FastCacheNodeUndoRegistration"|/>|Execute="rollback"
<Custom Action="FastCacheNodeUndoRegistration"|/>|After="FastCacheNodeRestoreRegistrationExactly"
<Custom Action="FastCacheNodeUndoRegistration"|/>|Condition="FASTCACHE_NODE_SELECTED = "1" AND NOT FASTCACHE_NODE_IMAGEPATH"
<CustomAction Id="FastCacheNodeRestoreRegistration"|/>|Execute="rollback"
<Custom Action="FastCacheNodeRestoreRegistration"|/>|After="FastCacheNodeUndoRegistration"
<Custom Action="FastCacheNodeRestoreRegistration"|/>|Condition="FASTCACHE_NODE_SELECTED = "1" AND FASTCACHE_NODE_IMAGEPATH"
<Custom Action="FastCacheNodeInstallService"|/>|After="FastCacheNodeRestoreRegistration"
<Custom Action="FastCacheNodeStartService"|/>|After="FastCacheNodeStopForRestart"

<SetProperty Action="SetFastCacheOwnImagePathPrefix"|/>|Value=""[INSTALL_ROOT]"
<SetProperty Action="SetFastCacheOwnImagePathPrefix"|/>|After="CostFinalize"
<SetProperty Action="SetFastCacheOwnImagePathPrefix"|/>|!Condition=
<SetProperty Action="SetFastCacheOwnImagePathRawPrefix"|/>|Value="#%"[INSTALL_ROOT]"
<SetProperty Action="SetFastCacheOwnImagePathRawPrefix"|/>|After="SetFastCacheOwnImagePathPrefix"
<SetProperty Action="SetFastCacheOwnImagePathRawPrefix"|/>|!Condition=

<SetProperty Action="ClearFastCacheNodeRegisteredHere"|/>|Value="0"
<SetProperty Action="ClearFastCacheNodeRegisteredHere"|/>|After="SetFastCacheOwnImagePathRawPrefix"
<SetProperty Action="ClearFastCacheNodeRegisteredHere"|/>|!Condition=
<SetProperty Action="SetFastCacheNodeRegisteredHereQuoted"|/>|Condition="FASTCACHE_OWN_IMAGEPATH_PREFIX AND FASTCACHE_NODE_IMAGEPATH ~<< FASTCACHE_OWN_IMAGEPATH_PREFIX"
<SetProperty Action="SetFastCacheNodeRegisteredHereQuoted"|/>|After="ClearFastCacheNodeRegisteredHere"
<SetProperty Action="SetFastCacheNodeRegisteredHereRaw"|/>|Condition="FASTCACHE_OWN_IMAGEPATH_RAW_PREFIX AND FASTCACHE_NODE_IMAGEPATH ~<< FASTCACHE_OWN_IMAGEPATH_RAW_PREFIX"
<SetProperty Action="SetFastCacheNodeRegisteredHereRaw"|/>|After="SetFastCacheNodeRegisteredHereQuoted"
<SetProperty Action="ClearFastCachedRegisteredHere"|/>|Value="0"
<SetProperty Action="ClearFastCachedRegisteredHere"|/>|After="SetFastCacheNodeRegisteredHereRaw"
<SetProperty Action="ClearFastCachedRegisteredHere"|/>|!Condition=
<SetProperty Action="SetFastCachedRegisteredHereQuoted"|/>|Condition="FASTCACHE_OWN_IMAGEPATH_PREFIX AND FASTCACHED_IMAGEPATH ~<< FASTCACHE_OWN_IMAGEPATH_PREFIX"
<SetProperty Action="SetFastCachedRegisteredHereQuoted"|/>|After="ClearFastCachedRegisteredHere"
<SetProperty Action="SetFastCachedRegisteredHereRaw"|/>|Condition="FASTCACHE_OWN_IMAGEPATH_RAW_PREFIX AND FASTCACHED_IMAGEPATH ~<< FASTCACHE_OWN_IMAGEPATH_RAW_PREFIX"
<SetProperty Action="SetFastCachedRegisteredHereRaw"|/>|After="SetFastCachedRegisteredHereQuoted"

<SetProperty Action="ClearFastCacheNodeSelected"|/>|Value="0"
<SetProperty Action="ClearFastCacheNodeSelected"|/>|After="MigrateFeatureStates"
<SetProperty Action="ClearFastCacheNodeSelected"|/>|!Condition=
<SetProperty Action="SetFastCacheNodeSelected"|/>|After="ClearFastCacheNodeSelected"
<SetProperty Action="ClearFastCachedSelected"|/>|Value="0"
<SetProperty Action="ClearFastCachedSelected"|/>|After="SetFastCacheNodeSelected"
<SetProperty Action="ClearFastCachedSelected"|/>|!Condition=
<SetProperty Action="SetFastCachedSelected"|/>|After="ClearFastCachedSelected"

<Custom Action="FastCacheNodeUninstallService"|/>|NOT UPGRADINGPRODUCTCODE

<CustomAction Id="FastCacheAwaitServiceExit"|/>|bin\fastcached.exe\"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|bin\fastcache-compile-node.exe\"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|exit 1460
<CustomAction Id="FastCacheAwaitServiceExit"|/>|Execute="deferred"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|Impersonate="no"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|Return="check"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|Directory="System64Folder"
<CustomAction Id="FastCacheAwaitServiceExit"|/>|!Directory="INSTALL_ROOT"
<Custom Action="FastCacheAwaitServiceExit"|/>|Before="InstallFiles"
<Custom Action="FastCacheAwaitServiceExit"|/>|Condition="WIX_UPGRADE_DETECTED AND NOT UPGRADINGPRODUCTCODE"
<CustomAction Id="FastCachedAwaitExitForNode"|/>|bin\fastcached.exe\"
<CustomAction Id="FastCachedAwaitExitForNode"|/>|!fastcache-compile-node
<CustomAction Id="FastCachedAwaitExitForNode"|/>|exit 1460
<CustomAction Id="FastCachedAwaitExitForNode"|/>|Return="check"
<Custom Action="FastCachedAwaitExitForNode"|/>|After="FastCachedStopForNode"
<Custom Action="FastCachedAwaitExitForNode"|/>|Condition="FASTCACHED_SELECTED = "1" AND FASTCACHE_NODE_SELECTED = "1""
<CustomAction Id="FastCachedStopForNode"|/>|/v FastCachedWasRunning
<CustomAction Id="FastCachedStopForNode"|/>|net.exe stop FastCached"
<CustomAction Id="FastCachedStopForNode"|/>|Return="ignore"
<Custom Action="FastCachedStopForNode"|/>|After="FastCachedInstallService"
<Custom Action="FastCachedStartService"|/>|After="FastCachedAwaitExitForNode"

<CustomAction Id="FastCacheClearRollbackState"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\InstallerRollback /f /reg:64"
<CustomAction Id="FastCacheClearRollbackState"|/>|Execute="deferred"
<Custom Action="FastCacheClearRollbackState"|/>|Before="FastCachedStashRegistration"
<CustomAction Id="FastCacheClearRollbackState"|/>|Directory="System64Folder"
<CustomAction Id="FastCacheDiscardRollbackState"|/>|reg.exe" delete HKLM\SOFTWARE\fastcached\InstallerRollback /f /reg:64"
<CustomAction Id="FastCacheDiscardRollbackState"|/>|Execute="deferred"
<Custom Action="FastCacheDiscardRollbackState"|/>|After="FastCacheNodeStartService"
<CustomAction Id="FastCachedRestartAfterStop"|/>|/v FastCachedWasRunning
<CustomAction Id="FastCachedRestartAfterStop"|/>|sc.exe start FastCached"
<CustomAction Id="FastCachedRestartAfterStop"|/>|Execute="rollback"
<Custom Action="FastCachedRestartAfterStop"|/>|After="FastCacheNodeCheckArguments"
<Custom Action="FastCachedRestartAfterStop"|/>|Condition="FASTCACHED_SELECTED = "1" AND FASTCACHE_NODE_SELECTED = "1""
<CustomAction Id="FastCacheNodeRestartAfterStop"|/>|/v NodeWasRunning
<CustomAction Id="FastCacheNodeRestartAfterStop"|/>|sc.exe start FastCacheCompileNode"
<CustomAction Id="FastCacheNodeRestartAfterStop"|/>|Execute="rollback"
<Custom Action="FastCacheNodeRestartAfterStop"|/>|After="FastCachedRestartAfterStop"
<Custom Action="FastCacheNodeRestartAfterStop"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""
<CustomAction Id="FastCacheNodeStopForRestart"|/>|/v NodeWasRunning
<CustomAction Id="FastCacheNodeStopForRestart"|/>|net.exe stop FastCacheCompileNode"
<CustomAction Id="FastCacheNodeStopForRestart"|/>|Return="ignore"
<Custom Action="FastCacheNodeStopForRestart"|/>|After="FastCacheForgetNodeAdvertise"
<Custom Action="FastCacheNodeStopForRestart"|/>|Condition="FASTCACHE_NODE_SELECTED = "1""

<Property Id="FASTCACHED_START_BEFORE"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCached"
<Property Id="FASTCACHED_START_BEFORE"|</Property>|Name="Start"
<Property Id="FASTCACHE_NODE_START_BEFORE"|</Property>|Key="SYSTEM\CurrentControlSet\Services\FastCacheCompileNode"
<Property Id="FASTCACHE_NODE_START_BEFORE"|</Property>|Name="Start"
<SetProperty Action="SetFastCachedStartModeBeforeAuto"|/>|Condition="FASTCACHED_START_BEFORE = "#2""
<SetProperty Action="SetFastCachedStartModeBeforeManual"|/>|Condition="NOT (FASTCACHED_START_BEFORE = "#2")"
<SetProperty Action="SetFastCacheNodeStartModeBeforeAuto"|/>|Condition="FASTCACHE_NODE_START_BEFORE = "#2""
<SetProperty Action="SetFastCacheNodeStartModeBeforeManual"|/>|Condition="NOT (FASTCACHE_NODE_START_BEFORE = "#2")"

<CustomAction Id="FastCachedStashRegistration"|/>|reg.exe" copy HKLM\SYSTEM\CurrentControlSet\Services\FastCached HKLM\SOFTWARE\fastcached\InstallerRollback\FastCached /s /f /reg:64"
<CustomAction Id="FastCachedStashRegistration"|/>|Execute="deferred"
<Custom Action="FastCachedStashRegistration"|/>|Before="FastCacheNodeStashRegistration"
<CustomAction Id="FastCachedStashRegistration"|/>|Directory="System64Folder"
<Custom Action="FastCachedStashRegistration"|/>|Condition="FASTCACHED_SELECTED = "1" AND FASTCACHED_IMAGEPATH"
<CustomAction Id="FastCachedRestoreRegistrationExactly"|/>|Rollback\FastCached'
<CustomAction Id="FastCachedRestoreRegistrationExactly"|/>|Invoke-CimMethod
<CustomAction Id="FastCachedRestoreRegistrationExactly"|/>|sc.exe config FastCached start=
<CustomAction Id="FastCachedRestoreRegistrationExactly"|/>|Execute="rollback"
<Custom Action="FastCachedRestoreRegistrationExactly"|/>|After="FastCacheNodeSeedConfig"
<Custom Action="FastCachedRestoreRegistrationExactly"|/>|Condition="FASTCACHED_SELECTED = "1" AND FASTCACHED_IMAGEPATH"
<CustomAction Id="FastCachedRestoreRegistration"|/>|fastcached.exe" --install-service --service-start=[FastCachedStartModeBefore] [FastCacheFirewallAllowBeforeArgument]"
<CustomAction Id="FastCachedRestoreRegistration"|/>|Execute="rollback"
<Custom Action="FastCachedRestoreRegistration"|/>|After="FastCachedUndoRegistration"
<Custom Action="FastCachedRestoreRegistration"|/>|Condition="FASTCACHED_SELECTED = "1" AND FASTCACHED_IMAGEPATH"
<CustomAction Id="FastCacheNodeStashRegistration"|/>|reg.exe" copy HKLM\SYSTEM\CurrentControlSet\Services\FastCacheCompileNode HKLM\SOFTWARE\fastcached\InstallerRollback\FastCacheCompileNode /s /f /reg:64"
<Custom Action="FastCacheNodeStashRegistration"|/>|Before="RemoveExistingProducts"
<CustomAction Id="FastCacheNodeStashRegistration"|/>|Directory="System64Folder"
<Custom Action="FastCacheNodeStashRegistration"|/>|Condition="FASTCACHE_NODE_SELECTED = "1" AND FASTCACHE_NODE_IMAGEPATH"
<CustomAction Id="FastCacheNodeRestoreRegistrationExactly"|/>|Rollback\FastCacheCompileNode'
<CustomAction Id="FastCacheNodeRestoreRegistrationExactly"|/>|sc.exe config FastCacheCompileNode start=
<CustomAction Id="FastCacheNodeRestoreRegistrationExactly"|/>|Execute="rollback"
<Custom Action="FastCacheNodeRestoreRegistrationExactly"|/>|After="FastCachedStartService"
<Custom Action="FastCacheNodeRestoreRegistrationExactly"|/>|Condition="FASTCACHE_NODE_SELECTED = "1" AND FASTCACHE_NODE_IMAGEPATH"
<CustomAction Id="FastCacheNodeRestoreRegistration"|/>|--service-start=[FastCacheNodeStartModeBefore]
<CustomAction Id="FastCacheNodeRestoreRegistration"|/>|!--service-start=auto
<Custom Action="FastCachedUninstallService"|/>|NOT UPGRADINGPRODUCTCODE
]=])

# The 0-then-1 flags rule 2 is about. Each must be read by at least one condition, or
# the rule is vacuous for it.
set(_flagProperties FASTCACHE_NODE_SELECTED FASTCACHED_SELECTED FASTCACHE_NODE_REGISTERED_HERE
    FASTCACHED_REGISTERED_HERE)

file(READ "${_file}" _text)

# Entities are decoded per extracted piece, never over the whole file: a decoded
# `&quot;` would end the attribute value the walk below is delimiting.
function(_fc_decode _in _out)
    string(REPLACE "&quot;" "\"" _s "${_in}")
    string(REPLACE "&gt;" ">" _s "${_s}")
    string(REPLACE "&lt;" "<" _s "${_s}")
    string(REPLACE "&amp;" "&" _s "${_s}")
    set(${_out} "${_s}" PARENT_SCOPE)
endfunction()

set(_offences "")
set(_rowCount 0)

set(_tableCursor 0)
string(LENGTH "${_table}" _tableLength)
while(_tableCursor LESS _tableLength)
    string(SUBSTRING "${_table}" ${_tableCursor} -1 _tableRest)
    string(FIND "${_tableRest}" "\n" _lineEnd)
    if(_lineEnd EQUAL -1)
        string(LENGTH "${_tableRest}" _lineEnd)
    endif()
    string(SUBSTRING "${_tableRest}" 0 ${_lineEnd} _row)
    math(EXPR _tableCursor "${_tableCursor} + ${_lineEnd} + 1")
    if(_row STREQUAL "")
        continue()
    endif()

    string(FIND "${_row}" "|" _bar1)
    if(_bar1 EQUAL -1)
        list(APPEND _offences "malformed table row (no |): ${_row}")
        continue()
    endif()
    string(SUBSTRING "${_row}" 0 ${_bar1} _opener)
    math(EXPR _after1 "${_bar1} + 1")
    string(SUBSTRING "${_row}" ${_after1} -1 _rest)
    string(FIND "${_rest}" "|" _bar2)
    if(_bar2 EQUAL -1)
        list(APPEND _offences "malformed table row (one |): ${_row}")
        continue()
    endif()
    string(SUBSTRING "${_rest}" 0 ${_bar2} _terminator)
    math(EXPR _after2 "${_bar2} + 1")
    string(SUBSTRING "${_rest}" ${_after2} -1 _required)
    math(EXPR _rowCount "${_rowCount} + 1")

    string(FIND "${_text}" "${_opener}" _first)
    string(FIND "${_text}" "${_opener}" _last REVERSE)
    if(_first EQUAL -1)
        list(APPEND _offences "missing: ${_opener}")
        continue()
    endif()
    if(NOT _first EQUAL _last)
        list(APPEND _offences "not unique: ${_opener}")
        continue()
    endif()

    string(SUBSTRING "${_text}" ${_first} -1 _fromOpener)
    string(FIND "${_fromOpener}" "${_terminator}" _end)
    if(_end EQUAL -1)
        list(APPEND _offences "never terminated by ${_terminator}: ${_opener}")
        continue()
    endif()
    string(SUBSTRING "${_fromOpener}" 0 ${_end} _element)
    _fc_decode("${_element}" _element)

    if(_required MATCHES "^!")
        string(SUBSTRING "${_required}" 1 -1 _forbidden)
        string(FIND "${_element}" "${_forbidden}" _hit)
        if(NOT _hit EQUAL -1)
            list(APPEND _offences "${_opener} must not carry: ${_forbidden}")
        endif()
    else()
        string(FIND "${_element}" "${_required}" _hit)
        if(_hit EQUAL -1)
            list(APPEND _offences "${_opener} lacks: ${_required}")
        endif()
    endif()
endwhile()

# Rule 2: a FIND/SUBSTRING walk over every Condition attribute, never a list split.
set(_conditions 0)
set(_readers "")
set(_cursor 0)
string(LENGTH "${_text}" _length)
while(_cursor LESS _length)
    string(SUBSTRING "${_text}" ${_cursor} -1 _tail)
    string(FIND "${_tail}" "Condition=\"" _at)
    if(_at EQUAL -1)
        break()
    endif()
    math(EXPR _valueStart "${_cursor} + ${_at} + 11")
    string(SUBSTRING "${_text}" ${_valueStart} -1 _valueTail)
    string(FIND "${_valueTail}" "\"" _valueEnd)
    if(_valueEnd EQUAL -1)
        list(APPEND _offences "a Condition attribute is never closed")
        break()
    endif()
    string(SUBSTRING "${_valueTail}" 0 ${_valueEnd} _condition)
    _fc_decode("${_condition}" _condition)
    math(EXPR _conditions "${_conditions} + 1")

    foreach(_property IN LISTS _flagProperties)
        set(_scan "${_condition}")
        while(TRUE)
            string(FIND "${_scan}" "${_property}" _p)
            if(_p EQUAL -1)
                break()
            endif()
            list(APPEND _readers "${_property}")
            string(LENGTH "${_property}" _nameLength)
            math(EXPR _next "${_p} + ${_nameLength}")
            string(SUBSTRING "${_scan}" ${_next} -1 _scan)
            string(FIND "${_scan}" " = \"1\"" _cmp)
            if(NOT _cmp EQUAL 0)
                list(APPEND _offences "a condition reads ${_property} without comparing it with \"1\": ${_condition}")
                break()
            endif()
        endwhile()
    endforeach()

    math(EXPR _cursor "${_valueStart} + ${_valueEnd} + 1")
endwhile()

# Positive controls: a walk that found nothing reports nothing wrong about it.
if(_conditions EQUAL 0)
    message(FATAL_ERROR
        "check-wix-service-table: found no Condition attribute in ${_file}, so rule 2 proved nothing rather than passing.")
endif()
foreach(_property IN LISTS _flagProperties)
    if(NOT _property IN_LIST _readers)
        list(APPEND _offences "no condition reads ${_property}, so rule 2 is vacuous for it")
    endif()
endforeach()

if(_offences)
    string(REPLACE ";" "\n  " _offenceText "${_offences}")
    message(FATAL_ERROR
        "check-wix-service-table: packaging/windows/service-actions.xml no longer carries the service "
        "table this check pins.\n"
        "  ${_offenceText}\n"
        "If the change is deliberate, change the row in scripts/check-wix-service-table.cmake in the "
        "same commit and say why there. A leftover-removal row lost its guard means the MSI can delete "
        "a service it did not register.")
endif()

message(STATUS
    "check-wix-service-table: ${_rowCount} row(s) and ${_conditions} condition(s) checked in service-actions.xml")
