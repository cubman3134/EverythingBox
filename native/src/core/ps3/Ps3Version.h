#pragma once
#include <QString>
// Numeric compare of Sony "NN.NN" version strings (01.05 < 01.11).
namespace Ps3Version { bool less(const QString& a, const QString& b); }
