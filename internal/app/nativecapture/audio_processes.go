package nativecapture

import (
	"context"
	"errors"
	"os/exec"
	"strings"
	"unicode/utf8"
)

type AudioProcess struct {
	PID   uint32 `json:"pid"`
	Name  string `json:"name"`
	Title string `json:"title,omitempty"`
}

func ListAudioProcesses(parent context.Context, executable string) ([]AudioProcess, error) {
	ctx, cancel := context.WithTimeout(parent, probeTimeout)
	defer cancel()
	stdout := &boundedBuffer{limit: maxProbeOutputBytes}
	command := exec.CommandContext(ctx, executable, "--list-audio-processes")
	command.Stdout = stdout
	hideWindow(command)
	if err := command.Run(); err != nil {
		return nil, errors.New("native audio process list is unavailable")
	}
	var processes []AudioProcess
	if err := decodeStrictJSON(stdout.Bytes(), &processes); err != nil || processes == nil || len(processes) > 256 {
		return nil, errors.New("native audio process list is invalid")
	}
	seen := make(map[uint32]bool, len(processes))
	for _, process := range processes {
		if process.PID == 0 || seen[process.PID] ||
			process.Name == "" || len(process.Name) > 512 || !utf8.ValidString(process.Name) || strings.ContainsRune(process.Name, 0) ||
			len(process.Title) > 512 || !utf8.ValidString(process.Title) || strings.ContainsRune(process.Title, 0) {
			return nil, errors.New("native audio process identity is invalid")
		}
		seen[process.PID] = true
	}
	return processes, nil
}
