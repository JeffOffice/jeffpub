#pragma once
// Anonymous usage statistics. Once a day (and right away after an update),
// while JeffPub 79 runs and unless
// the person using it turned them off (in setup, when it first starts, or
// in File > Options), it sends a random install id made up here, its version,
// the operating system and language, how many times it was started, and how
// many times each command was used. Never files, their names or their text.
// The collector (server/telemetry) keeps no IP address.

#include <QJsonObject>
#include <QString>

class QWidget;

namespace jp::telemetry {

bool decided();                         // the person has chosen (or setup chose for them)
bool enabled();
void setEnabled(bool on);
void count(const QString &feature);     // a command's id, e.g. "file.open.pub"; nothing when off
QJsonObject payload();                  // what the next ping would send
bool due();                             // on, and no ping yet today or from this version
void start(QWidget *window);            // asks once, then pings daily; only main() calls this

} // namespace jp::telemetry
