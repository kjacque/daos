//
// (C) Copyright 2019 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//
package scm

import (
	"encoding/json"
	"fmt"
	"os/exec"
	"strings"

	"github.com/pkg/errors"

	"github.com/daos-stack/daos/src/control/lib/ipmctl"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/server/storage"
)

const (
	cmdScmShowRegions = "ipmctl show -d PersistentMemoryType,FreeCapacity -region"
	outScmNoRegions   = "\nThere are no Regions defined in the system.\n"
	// creates a AppDirect/Interleaved memory allocation goal across all DCPMMs on a system.
	cmdScmCreateRegions    = "ipmctl create -f -goal PersistentMemoryType=AppDirect"
	cmdScmRemoveRegions    = "ipmctl create -f -goal MemoryMode=100"
	cmdScmDeleteGoal       = "ipmctl delete -goal"
	cmdScmCreateNamespace  = "ndctl create-namespace" // returns json ns info
	cmdScmListNamespaces   = "ndctl list -N -v"       // returns json ns info
	cmdScmDisableNamespace = "ndctl disable-namespace %s"
	cmdScmDestroyNamespace = "ndctl destroy-namespace %s"
)

type runCmdFn func(string) (string, error)
type lookPathFn func(string) (string, error)

type runCmdError struct {
	wrapped error
	stdout  string
}

func (rce *runCmdError) Error() string {
	if ee, ok := rce.wrapped.(*exec.ExitError); ok {
		return fmt.Sprintf("%s: stdout: %s; stderr: %s", ee.ProcessState,
			rce.stdout, ee.Stderr)
	}

	return fmt.Sprintf("%s: stdout: %s", rce.wrapped.Error(), rce.stdout)
}

func run(cmd string) (string, error) {
	out, err := exec.Command("bash", "-c", cmd).Output()
	if err != nil {
		return "", &runCmdError{
			wrapped: err,
			stdout:  string(out),
		}
	}

	return string(out), nil
}

type cmdRunner struct {
	log      logging.Logger
	binding  ipmctl.IpmCtl
	runCmd   runCmdFn
	lookPath lookPathFn
}

// checkNdctl verifies ndctl application is installed.
func (r *cmdRunner) checkNdctl() error {
	_, err := r.lookPath("ndctl")
	if err != nil {
		return FaultMissingNdctl
	}

	return nil
}

func (r *cmdRunner) Discover() (storage.ScmModules, error) {
	discovery, err := r.binding.Discover()
	if err != nil {
		return nil, errors.Wrap(err, "failed to discover SCM modules")
	}
	r.log.Debugf("discovered %d DCPM modules", len(discovery))

	modules := make(storage.ScmModules, 0, len(discovery))
	for _, d := range discovery {
		modules = append(modules, storage.ScmModule{
			ChannelID:       uint32(d.Channel_id),
			ChannelPosition: uint32(d.Channel_pos),
			ControllerID:    uint32(d.Memory_controller_id),
			SocketID:        uint32(d.Socket_id),
			PhysicalID:      uint32(d.Physical_id),
			Capacity:        d.Capacity,
		})
	}

	return modules, nil
}

// getState establishes state of SCM regions and namespaces on local server.
func (r *cmdRunner) GetState() (storage.ScmState, error) {
	if err := r.checkNdctl(); err != nil {
		return storage.ScmStateUnknown, err
	}

	// TODO: discovery should provide SCM region details
	out, err := r.runCmd(cmdScmShowRegions)
	if err != nil {
		return storage.ScmStateUnknown, err
	}

	r.log.Debugf("show region output: %s\n", out)

	if out == outScmNoRegions {
		return storage.ScmStateNoRegions, nil
	}

	ok, err := hasFreeCapacity(out)
	if err != nil {
		return storage.ScmStateUnknown, err
	}
	if ok {
		return storage.ScmStateFreeCapacity, nil
	}

	return storage.ScmStateNoCapacity, nil
}

// Prep executes commands to configure SCM modules into AppDirect interleaved
// regions/sets hosting pmem device file namespaces.
//
// Presents of nonvolatile memory modules is assumed in this method and state
// is established based on presence and free capacity of regions.
//
// Actions based on state:
// * modules exist and no regions -> create all regions (needs reboot)
// * regions exist and free capacity -> create all namespaces, return created
// * regions exist but no free capacity -> no-op, return namespaces
//
// Command output from external tools will be returned. State will be passed in.
func (r *cmdRunner) Prep(state storage.ScmState) (needsReboot bool, pmemDevs storage.ScmNamespaces, err error) {
	if err = r.checkNdctl(); err != nil {
		return
	}

	r.log.Debugf("scm in state %s\n", state)

	switch state {
	case storage.ScmStateNoRegions:
		// clear any pre-existing goals first
		if _, err = r.runCmd(cmdScmDeleteGoal); err != nil {
			err = errors.WithMessage(err, "clear goal")
			return
		}
		// if successful, memory allocation change read on reboot
		if _, err = r.runCmd(cmdScmCreateRegions); err == nil {
			needsReboot = true
		}
	case storage.ScmStateFreeCapacity:
		pmemDevs, err = r.createNamespaces()
	case storage.ScmStateNoCapacity:
		pmemDevs, err = r.GetNamespaces()
	case storage.ScmStateUnknown:
		err = errors.New("unknown scm state")
	default:
		err = errors.Errorf("unhandled scm state %q", state)
	}

	return
}

// PrepReset executes commands to remove namespaces and regions on SCM modules.
//
// Returns indication of whether a reboot is required alongside error.
// Command output from external tools will be returned. State will be passed in.
func (r *cmdRunner) PrepReset(state storage.ScmState) (bool, error) {
	if err := r.checkNdctl(); err != nil {
		return false, nil
	}

	r.log.Debugf("scm in state %s\n", state)

	switch state {
	case storage.ScmStateNoRegions:
		r.log.Info("SCM is already reset\n")
		return false, nil
	case storage.ScmStateFreeCapacity, storage.ScmStateNoCapacity:
	case storage.ScmStateUnknown:
		return false, errors.New("unknown scm state")
	default:
		return false, errors.Errorf("unhandled scm state %q", state)
	}

	namespaces, err := r.GetNamespaces()
	if err != nil {
		return false, err
	}

	for _, dev := range namespaces {
		if err := r.removeNamespace(dev.Name); err != nil {
			return false, err
		}
	}

	r.log.Infof("resetting SCM memory allocations\n")
	// clear any pre-existing goals first
	if _, err := r.runCmd(cmdScmDeleteGoal); err != nil {
		return false, err
	}
	if out, err := r.runCmd(cmdScmRemoveRegions); err != nil {
		r.log.Error(out)
		return false, err
	}

	return true, nil // memory allocation reset requires a reboot
}

func (r *cmdRunner) removeNamespace(devName string) (err error) {
	r.log.Infof("removing SCM namespace %q, may take a few minutes...\n", devName)

	_, err = r.runCmd(fmt.Sprintf(cmdScmDisableNamespace, devName))
	if err != nil {
		return
	}

	_, err = r.runCmd(fmt.Sprintf(cmdScmDestroyNamespace, devName))
	if err != nil {
		return
	}

	return
}

// hasFreeCapacity takes output from ipmctl and checks for free capacity.
//
// external tool commands return:
// $ ipmctl show -d PersistentMemoryType,FreeCapacity -region
//
// ---ISetID=0x2aba7f4828ef2ccc---
//    PersistentMemoryType=AppDirect
//    FreeCapacity=3012.0 GiB
// ---ISetID=0x81187f4881f02ccc---
//    PersistentMemoryType=AppDirect
//    FreeCapacity=3012.0 GiB
//
// FIXME: implementation to be replaced by using libipmctl directly through bindings
func hasFreeCapacity(text string) (hasCapacity bool, err error) {
	lines := strings.Split(text, "\n")
	if len(lines) < 4 {
		return false, errors.Errorf("expecting at least 4 lines, got %d",
			len(lines))
	}

	for _, line := range lines {
		entry := strings.TrimSpace(line)

		kv := strings.Split(entry, "=")
		if len(kv) != 2 {
			continue
		}

		if kv[0] == "PersistentMemoryType" && kv[1] == "AppDirect" {
			hasCapacity = true
			continue
		}

		if kv[0] != "FreeCapacity" {
			continue
		}

		if hasCapacity && kv[1] != "0.0 GiB" {
			return
		}

		hasCapacity = false
	}

	return
}

// createstorage.ScmNamespaces runs create until no free capacity.
func (r *cmdRunner) createNamespaces() (storage.ScmNamespaces, error) {
	devs := make(storage.ScmNamespaces, 0)

	for {
		r.log.Infof("creating SCM namespace, may take a few minutes...\n")

		out, err := r.runCmd(cmdScmCreateNamespace)
		if err != nil {
			return nil, errors.WithMessage(err, "create namespace cmd")
		}

		newDevs, err := parseNamespaces(out)
		if err != nil {
			return nil, errors.WithMessage(err, "parsing pmem devs")
		}
		devs = append(devs, newDevs...)

		state, err := r.GetState()
		if err != nil {
			return nil, errors.WithMessage(err, "getting state")
		}

		switch state {
		case storage.ScmStateNoCapacity:
			return devs, nil
		case storage.ScmStateFreeCapacity:
		default:
			return nil, errors.Errorf("unexpected state: want %s, got %s",
				storage.ScmStateFreeCapacity.String(), state.String())
		}
	}
}

func (r *cmdRunner) GetNamespaces() (storage.ScmNamespaces, error) {
	if err := r.checkNdctl(); err != nil {
		return nil, err
	}

	out, err := r.runCmd(cmdScmListNamespaces)
	if err != nil {
		return nil, err
	}

	nss, err := parseNamespaces(out)
	if err != nil {
		return nil, err
	}

	r.log.Debugf("discovered %d DCPM namespaces", len(nss))
	return nss, nil
}

func parseNamespaces(jsonData string) (nss storage.ScmNamespaces, err error) {
	// turn single entries into arrays
	if !strings.HasPrefix(jsonData, "[") {
		jsonData = "[" + jsonData + "]"
	}

	err = json.Unmarshal([]byte(jsonData), &nss)

	return
}

func defaultCmdRunner(log logging.Logger) *cmdRunner {
	return newCmdRunner(log, &ipmctl.NvmMgmt{}, run, exec.LookPath)
}

func newCmdRunner(log logging.Logger, lib ipmctl.IpmCtl, runCmd runCmdFn, lookPath lookPathFn) *cmdRunner {
	return &cmdRunner{
		log:      log,
		binding:  lib,
		runCmd:   runCmd,
		lookPath: lookPath,
	}
}
