#include "core/project_format.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>

namespace infalsus {

namespace {

constexpr int kOggHeaderBytes = 27;
constexpr quint32 kOggPolynomial = 79764919;
constexpr quint32 kPngPolynomial = 3988292384U;

[[nodiscard]] bool validPngChunks(const QByteArray& jacket) {
    qsizetype cursor = 8;
    bool imageData = false;

    while (jacket.size() - cursor >= 12) {
        const auto* bytes = reinterpret_cast<const uchar*>(jacket.constData() + cursor);
        const quint32 length = qFromBigEndian<quint32>(bytes);
        if (length > quint64(jacket.size() - cursor - 12)) {
            return false;
        }
        const QByteArray type = jacket.mid(cursor + 4, 4);
        if ((cursor == 8 && (type != "IHDR" || length != 13)) || (cursor != 8 && type == "IHDR")) {
            return false;
        }
        quint32 checksum = 4294967295U;
        for (quint64 index = 4; index < quint64(length) + 8; ++index) {
            checksum ^= bytes[index];
            for (int bit = 0; bit < 8; ++bit) {
                checksum = (checksum >> 1) ^ ((checksum & 1) ? kPngPolynomial : 0);
            }
        }
        if (~checksum != qFromBigEndian<quint32>(bytes + 8 + length)) {
            return false;
        }
        imageData |= type == "IDAT";
        cursor += qsizetype(length) + 12;
        if (type == "IEND") {
            return imageData && length == 0 && cursor == jacket.size();
        }
    }

    return false;
}

[[nodiscard]] bool validVorbis(const QByteArray& audio) {
    QByteArray packet;
    qsizetype cursor = 0;
    quint32 serial = 0;
    quint32 sequence = 0;
    int packetIndex = 0;
    bool ended = false;

    while (cursor < audio.size()) {
        if (ended || audio.size() - cursor < kOggHeaderBytes || audio.mid(cursor, 4) != "OggS"
            || audio.at(cursor + 4) != 0) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const uchar*>(audio.constData() + cursor);
        const int flags = bytes[5];
        const int segmentCount = bytes[26];
        const quint32 pageSerial = qFromLittleEndian<quint32>(bytes + 14);
        const quint32 pageSequence = qFromLittleEndian<quint32>(bytes + 18);
        qsizetype pageLength = kOggHeaderBytes + segmentCount;
        if (audio.size() - cursor < pageLength || (flags & ~7) != 0
            || bool(flags & 1) != !packet.isEmpty()) {
            return false;
        }
        if (cursor == 0) {
            serial = pageSerial;
            if ((flags & 2) == 0 || pageSequence != 0) {
                return false;
            }
        } else if ((flags & 2) != 0) {
            return false;
        }
        if (pageSerial != serial || pageSequence != sequence++) {
            return false;
        }
        for (int index = 0; index < segmentCount; ++index) {
            pageLength += bytes[kOggHeaderBytes + index];
        }
        if (audio.size() - cursor < pageLength) {
            return false;
        }
        quint32 checksum = 0;
        for (qsizetype index = 0; index < pageLength; ++index) {
            checksum ^= quint32(index >= 22 && index < 26 ? 0 : bytes[index]) << 24;
            for (int bit = 0; bit < 8; ++bit) {
                checksum = (checksum << 1) ^ ((checksum & 2147483648U) ? kOggPolynomial : 0);
            }
        }
        if (checksum != qFromLittleEndian<quint32>(bytes + 22)) {
            return false;
        }
        qsizetype payload = cursor + kOggHeaderBytes + segmentCount;
        for (int index = 0; index < segmentCount; ++index) {
            const int length = bytes[kOggHeaderBytes + index];
            if (packetIndex < 3) {
                packet.append(audio.constData() + payload, length);
            } else if (length == 255 && packet.isEmpty()) {
                packet.append(char(0));
            }
            payload += length;
            if (length < 255) {
                if (packetIndex < 3) {
                    const int type = packetIndex == 0 ? 1 : packetIndex == 1 ? 3 : 5;
                    if (packet.size() < 7 || quint8(packet.at(0)) != type || packet.mid(1, 6) != "vorbis") {
                        return false;
                    }
                    if (packetIndex == 0 && (packet.size() != 30
                        || qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(packet.constData()) + 7) != 0
                        || quint8(packet.at(11)) == 0
                        || qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(packet.constData()) + 12) == 0
                        || (quint8(packet.at(29)) & 1) == 0)) {
                        return false;
                    }
                }
                ++packetIndex;
                packet.clear();
            }
        }
        ended = (flags & 4) != 0;
        cursor += pageLength;
    }

    return ended && packet.isEmpty() && packetIndex > 3;
}

} // namespace

bool isValidProjectChartId(const QString& chartId) {
    static const QRegularExpression kAllowed(QStringLiteral("\\A[A-Za-z0-9_-]+\\z"));
    static const QRegularExpression kReserved(QStringLiteral("\\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\\z"),
        QRegularExpression::CaseInsensitiveOption);

    return chartId.size() <= 200 && kAllowed.match(chartId).hasMatch() && !kReserved.match(chartId).hasMatch();
}

bool isProjectVorbisAudio(const QByteArray& audio) {
    return validVorbis(audio);
}

bool validateProjectMedia(const QByteArray& audio, const QByteArray& jacket, QString* error) {
    if (!validVorbis(audio)) {
        *error = QStringLiteral("The project audio must be a complete Ogg Vorbis stream.");
        return false;
    }
    const QByteArray signature = QByteArray::fromHex("89504e470d0a1a0a");
    if (jacket.size() < 33 || !jacket.startsWith(signature) || jacket.mid(12, 4) != "IHDR"
        || quint8(jacket.at(24)) != 8 || quint8(jacket.at(25)) != 6
        || jacket.at(26) != 0 || jacket.at(27) != 0 || jacket.at(28) != 0) {
        *error = QStringLiteral("The project jacket must be a non-interlaced 8-bit RGBA PNG.");
        return false;
    }
    const auto* bytes = reinterpret_cast<const uchar*>(jacket.constData());
    const quint64 width = qFromBigEndian<quint32>(bytes + 16);
    const quint64 height = qFromBigEndian<quint32>(bytes + 20);
    if (width == 0 || height == 0 || width * height > kMaximumProjectJacketPixels
        || !validPngChunks(jacket)) {
        *error = QStringLiteral("The project jacket dimensions or PNG data are invalid.");
        return false;
    }

    return true;
}

} // namespace infalsus
