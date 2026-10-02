import { useCopy } from "../../ui/copy";
import { Glyph } from "../../ui/icons";
import { Tooltip } from "./Tooltip";
import type { NativeAudioProcess } from "../../native/wire";

export function HostAudioProcesses({
  processes,
  excludedPids,
  onToggleExclude,
  onRefresh,
  refreshing = false,
  disabled = false,
  collapsible = false,
}: {
  processes: NativeAudioProcess[];
  excludedPids: number[];
  onToggleExclude: (pid: number) => void;
  onRefresh?: () => void;
  refreshing?: boolean;
  disabled?: boolean;
  collapsible?: boolean;
}) {
  const { t, vis } = useCopy();

  const countBadge = excludedPids.length > 0
    ? ` (${t("host.sourcePicker.audioProcessesCount", { count: String(excludedPids.length) })})`
    : "";

  const content = (
    <>
      <div className="lr-audio-processes-header">
        <span className="lr-audio-processes-title">
          <Glyph name="speaker" size={17} />
          {vis ? null : <strong>{t("host.sourcePicker.audioProcesses")}{countBadge}</strong>}
        </span>
        {onRefresh ? (
          <Tooltip kind="hint-refresh-audio-processes" text={vis ? undefined : t("host.sourcePicker.refreshAudioProcesses")} place="below" align="end">
            <button
              type="button"
              className="lr-btn lr-btn-ghost lr-device-refresh"
              disabled={refreshing || disabled}
              aria-label={t("host.sourcePicker.refreshAudioProcesses")}
              onClick={onRefresh}
            >
              <Glyph name="refresh" size={16} className={refreshing ? "lr-spin" : undefined} />
            </button>
          </Tooltip>
        ) : null}
      </div>
      {vis ? null : (
        <p className="lr-audio-processes-hint">{t("host.sourcePicker.audioProcessesHint")}</p>
      )}
      {processes.length === 0 ? (
        <div className="lr-audio-processes-empty" role="status">
          <small>{t("host.sourcePicker.noAudioProcesses")}</small>
        </div>
      ) : (
        <ul className="lr-audio-processes-list" role="list">
          {processes.map((proc) => {
            const isExcluded = excludedPids.includes(proc.pid);
            const statusLabel = isExcluded
              ? t("host.sourcePicker.audioProcessesExclude")
              : t("host.sourcePicker.audioProcessesInclude");
            return (
              <li key={proc.pid} className={`lr-audio-process-item${isExcluded ? " is-excluded" : ""}`}>
                <div className="lr-audio-process-info">
                  <span className="lr-audio-process-name" title={proc.name}>
                    {proc.name}
                  </span>
                  {proc.title ? (
                    <span className="lr-audio-process-sub" title={proc.title}>
                      {proc.title}
                    </span>
                  ) : null}
                </div>
                <Tooltip kind="hint-toggle-process-audio" text={vis ? undefined : statusLabel} place="left">
                  <button
                    type="button"
                    className="lr-switch"
                    role="switch"
                    aria-checked={!isExcluded}
                    aria-label={`${proc.name}: ${statusLabel}`}
                    disabled={disabled}
                    onClick={() => onToggleExclude(proc.pid)}
                  />
                </Tooltip>
              </li>
            );
          })}
        </ul>
      )}
    </>
  );

  if (collapsible) {
    return (
      <details className="lr-audio-processes-details">
        <summary className="lr-audio-processes-summary">
          <span className="lr-audio-processes-summary-label">
            <Glyph name="speaker" size={17} />
            {vis ? null : <span>{t("host.sourcePicker.audioProcesses")}{countBadge}</span>}
          </span>
        </summary>
        <div className="lr-audio-processes-content">
          {content}
        </div>
      </details>
    );
  }

  return (
    <div className="lr-door-group lr-audio-processes-group" role="group" aria-label={t("host.sourcePicker.audioProcesses")}>
      {content}
    </div>
  );
}
