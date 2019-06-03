/* Webcamoid, webcam capture application.
 * Copyright (C) 2016  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <QDebug>
#include <QGenericMatrix>
#include <QMetaEnum>
#include <QVariant>
#include <QtEndian>
#include <QtMath>

#include "akaudiopacket.h"
#include "akpacket.h"
#include "akcaps.h"
#include "akfrac.h"

using AudioConvertFuntion =
    std::function<AkAudioPacket (const AkAudioPacket &src)>;

class AkAudioPacketPrivate
{
    public:
        AkAudioCaps m_caps;
        QByteArray m_buffer;
        qint64 m_pts {0};
        AkFrac m_timeBase;
        qint64 m_id {-1};
        int m_index {-1};

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValue(InputType value)
        {
            InputType xmin;
            InputType xmax;

            if (typeid(InputType) == typeid(float)) {
                value = qBound<InputType>(InputType(-1.0f),
                                          value,
                                          InputType(1.0f));
                xmin = InputType(-1.0f);
                xmax = InputType(1.0f);
            } else if (typeid(InputType) == typeid(qreal)) {
                value = qBound<InputType>(InputType(-1.0),
                                          value,
                                          InputType(1.0));
                xmin = InputType(-1.0);
                xmax = InputType(1.0);
            } else {
                xmin = std::numeric_limits<InputType>::min();
                xmax = std::numeric_limits<InputType>::max();
            }

            OutputType ymin;
            OutputType ymax;

            if (typeid(OutputType) == typeid(float)) {
                ymin = OutputType(-1.0f);
                ymax = OutputType(1.0f);
            } else if (typeid(InputType) == typeid(qreal)) {
                ymin = OutputType(-1.0);
                ymax = OutputType(1.0);
            } else {
                ymin = std::numeric_limits<OutputType>::min();
                ymax = std::numeric_limits<OutputType>::max();
            }

            return OutputType((OpType(value - xmin) * OpType(ymax - ymin)
                               + OpType(ymin) * OpType(xmax - xmin))
                              / OpType(xmax - xmin));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValue(InputType value,
                                            InputType minValue,
                                            InputType maxValue)
        {
            auto ymin = std::numeric_limits<OutputType>::min();
            auto ymax = std::numeric_limits<OutputType>::max();

            return OutputType((OpType(value - minValue) * OpType(ymax - ymin)
                               + OpType(ymin) * OpType(maxValue - minValue))
                              / OpType(maxValue - minValue));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFrom_To_(InputType value)
        {
            return scaleValue<InputType, OutputType, OpType>(value);
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFrom_ToLE(InputType value)
        {
            return qToLittleEndian(scaleValue<InputType,
                                              OutputType,
                                              OpType>(value));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFrom_ToBE(InputType value)
        {
            return qToLittleEndian(scaleValue<InputType,
                                              OutputType,
                                              OpType>(value));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromLETo_(InputType value)
        {
            return scaleValue<InputType,
                              OutputType,
                              OpType>(qFromLittleEndian(value));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromLEToLE(InputType value)
        {
            return qToLittleEndian(scaleValue
                                   <InputType,
                                    OutputType,
                                    OpType>(qFromLittleEndian(value)));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromLEToBE(InputType value)
        {
            return qToBigEndian(scaleValue
                                <InputType,
                                 OutputType,
                                 OpType>(qFromLittleEndian(value)));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromBETo_(InputType value)
        {
            return scaleValue<InputType,
                              OutputType,
                              OpType>(qFromBigEndian(value));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromBEToLE(InputType value)
        {
            return qToLittleEndian(scaleValue<InputType,
                                              OutputType,
                                              OpType>(qFromBigEndian(value)));
        }

        template<typename InputType, typename OutputType, typename OpType>
        inline static OutputType scaleValueFromBEToBE(InputType value)
        {
            return qToBigEndian(scaleValue<InputType,
                                           OutputType,
                                           OpType>(qFromBigEndian(value)));
        }

        template<typename InputType,
                 typename OutputType,
                 typename ScaleFunc>
        inline static AkAudioPacket convertSampleFormat(const AkAudioPacket &src,
                                                        AkAudioCaps::SampleFormat format,
                                                        ScaleFunc scaleFunc)
        {
            auto caps = src.caps();
            caps.setFormat(format);
            AkAudioPacket dst(caps);
            dst.copyMetadata(src);

            for (int plane = 0; plane < caps.planes(); plane++) {
                auto src_line = reinterpret_cast<const InputType *>(src.constPlaneData(plane));
                auto dst_line = reinterpret_cast<OutputType *>(dst.planeData(plane));

                for (int i = 0; i < caps.samples(); i++)
                    dst_line[i] = scaleFunc(src_line[i]);
            }

            return dst;
        }

#define DEFINE_SAMPLE_CONVERT_FUNCTION(sitype, \
                                       sotype, \
                                       itype, \
                                       otype, \
                                       optype, \
                                       inEndian, \
                                       outEndian) \
        {AkAudioCaps::SampleFormat_##sitype, \
         AkAudioCaps::SampleFormat_##sotype, \
         [] (const AkAudioPacket &src) -> AkAudioPacket { \
            return convertSampleFormat<itype, otype> \
                    (src, \
                     AkAudioCaps::SampleFormat_##sotype, \
                     scaleValueFrom##inEndian##To##outEndian<itype, \
                                                             otype, \
                                                             optype>); \
         }}, \
        {AkAudioCaps::SampleFormat_##sotype, \
         AkAudioCaps::SampleFormat_##sitype, \
         [] (const AkAudioPacket &src) -> AkAudioPacket { \
             return convertSampleFormat<otype, itype> \
                    (src, \
                     AkAudioCaps::SampleFormat_##sitype, \
                     scaleValueFrom##outEndian##To##inEndian<otype, \
                                                             itype, \
                                                             optype>); \
         }}

        struct AudioSampleFormatConvert
        {
            AkAudioCaps::SampleFormat from;
            AkAudioCaps::SampleFormat to;
            AudioConvertFuntion convert;
        };

        using AudioSampleFormatConvertFuncs = QVector<AudioSampleFormatConvert>;

        inline static const AudioSampleFormatConvertFuncs &sampleFormatConvert()
        {
            // Convert sample formats
            static const AudioSampleFormatConvertFuncs convert {
                DEFINE_SAMPLE_CONVERT_FUNCTION(s8   , s64,   qint8, qint64, qint64,  _, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u8   , s64,  quint8, qint64, qint64,  _, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s16le, s64,  qint16, qint64, qint64, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s16be, s64,  qint16, qint64, qint64, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u16le, s64, quint16, qint64, qint64, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u16be, s64, quint16, qint64, qint64, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s32le, s64,  qint32, qint64, qint64, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s32be, s64,  qint32, qint64, qint64, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u32le, s64, quint32, qint64, qint64, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u32be, s64, quint32, qint64, qint64, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s64le, s64,  qint64, qint64,  qreal, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(s64be, s64,  qint64, qint64,  qreal, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u64le, s64, quint64, qint64,  qreal, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(u64be, s64, quint64, qint64,  qreal, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(fltle, s64,   float, qint64,  qreal, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(fltbe, s64,   float, qint64,  qreal, BE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(dblle, s64,   qreal, qint64,  qreal, LE, _),
                DEFINE_SAMPLE_CONVERT_FUNCTION(dblbe, s64,   qreal, qint64,  qreal, BE, _),
            };

            return convert;
        }

        template<typename SampleType>
        inline static void waveBounds(AkAudioPacket packet,
                                      SampleType &smin,
                                      SampleType &smax)
        {
            // Find the minimum and maximum values of the sum.
            smin = std::numeric_limits<SampleType>::max();
            smax = std::numeric_limits<SampleType>::min();

            for (int channel = 0; channel < packet.caps().channels(); channel++) {
                for (int sample = 0; sample < packet.caps().samples(); sample++) {
                    auto data =
                            *reinterpret_cast<const SampleType *>(packet.constSample(channel,
                                                                                     sample));

                    if (data < smin)
                        smin = data;

                    if (data > smax)
                        smax = data;
                }
            }

            // Limit the maximum and the minimum of the wave so it won't get
            // out of the bounds.
            SampleType minValue;
            SampleType maxValue;

            if (typeid(SampleType) == typeid(float)) {
                minValue = SampleType(-1.0f);
                maxValue = SampleType(1.0f);
            } else if (typeid(SampleType) == typeid(qreal)) {
                minValue = SampleType(-1.0);
                maxValue = SampleType(1.0);
            } else {
                minValue = std::numeric_limits<SampleType>::min();
                maxValue = std::numeric_limits<SampleType>::max();
            }

            if (smin > SampleType(minValue))
                smin = SampleType(minValue);

            if (smax < SampleType(maxValue))
                smax = SampleType(maxValue);
        }

        template<typename SampleType, typename SumType>
        inline static AkAudioPacket mixChannels_(AkAudioCaps::SampleFormat sumFormat,
                                                 AkAudioCaps::ChannelLayout outputLayout,
                                                 const AkAudioPacket &src)
        {
            // Create a summatory packet which type is big enough to contain
            // the sum of all values.
            auto caps = src.caps();
            caps.setFormat(sumFormat);
            caps.setLayout(outputLayout);
            AkAudioPacket sumPacket(caps);
            sumPacket.buffer().fill(0);

            for (int sample = 0; sample < caps.samples(); sample++) {
                for (int ochannel = 0; ochannel < caps.channels(); ochannel++) {
                    auto oposition = sumPacket.caps().position(ochannel);

                    for (int ichannel = 0; ichannel < src.caps().channels(); ichannel++) {
                        /* We use inverse square law to sum the samples
                         * according to the speaker position in the sound dome.
                         *
                         * http://digitalsoundandmusic.com/4-3-4-the-mathematics-of-the-inverse-square-law-and-pag-equations/
                         */
                        auto iposition = src.caps().position(ichannel);
                        auto d = 1.0 + (oposition - iposition);
                        auto k = d * d;

                        auto inSample =
                                reinterpret_cast<const SampleType *>(src.constSample(ichannel,
                                                                                     sample));
                        auto outSample =
                                reinterpret_cast<SumType *>(sumPacket.sample(ochannel,
                                                                             sample));
                        *outSample += SumType(qreal(*inSample) / k);
                    }
                }
            }

            // Calculate minimum and maximum values of the wave.
            SumType smin;
            SumType smax;
            waveBounds<SumType>(sumPacket, smin, smax);

            caps = src.caps();
            caps.setLayout(outputLayout);
            AkAudioPacket dst(caps);
            dst.copyMetadata(src);

            // Recreate frame with the wave scaled to fit it.
            for (int channel = 0; channel < dst.caps().channels(); channel++) {
                for (int sample = 0; sample < dst.caps().samples(); sample++) {
                    auto idata =
                            reinterpret_cast<const SumType *>(sumPacket.constSample(channel,
                                                                                    sample));
                    auto odata =
                            reinterpret_cast<SampleType *>(dst.sample(channel,
                                                                      sample));

                    *odata = scaleValue<SumType, SampleType, SumType>(*idata,
                                                                      smin,
                                                                      smax);
                }
            }

            return dst;
        }

        template<typename SampleType, typename SumType>
        inline static AkAudioPacket mixChannelsLE(AkAudioCaps::SampleFormat sumFormat,
                                                  AkAudioCaps::ChannelLayout outputLayout,
                                                  const AkAudioPacket &src)
        {
            // Create a summatory packet which type is big enough to contain
            // the sum of all values.
            auto caps = src.caps();
            caps.setFormat(sumFormat);
            caps.setLayout(outputLayout);
            AkAudioPacket sumPacket(caps);
            sumPacket.buffer().fill(0);

            for (int sample = 0; sample < caps.samples(); sample++) {
                for (int ochannel = 0; ochannel < caps.channels(); ochannel++) {
                    auto oposition = sumPacket.caps().position(ochannel);

                    for (int ichannel = 0; ichannel < src.caps().channels(); ichannel++) {
                        /* We use inverse square law to sum the samples
                         * according to the speaker position in the sound dome.
                         *
                         * http://digitalsoundandmusic.com/4-3-4-the-mathematics-of-the-inverse-square-law-and-pag-equations/
                         */
                        auto iposition = src.caps().position(ichannel);
                        auto d = 1.0 + (oposition - iposition);
                        auto k = d * d;

                        auto inSample =
                                reinterpret_cast<const SampleType *>(src.constSample(ichannel,
                                                                                     sample));
                        auto outSample =
                                reinterpret_cast<SumType *>(sumPacket.sample(ochannel,
                                                                             sample));
                        *outSample += SumType(qreal(qFromLittleEndian(*inSample)) / k);
                    }
                }
            }

            // Calculate minimum and maximum values of the wave.
            SumType smin;
            SumType smax;
            waveBounds<SumType>(sumPacket, smin, smax);

            caps = src.caps();
            caps.setLayout(outputLayout);
            AkAudioPacket dst(caps);
            dst.copyMetadata(src);

            // Recreate frame with the wave scaled to fit it.
            for (int channel = 0; channel < dst.caps().channels(); channel++) {
                for (int sample = 0; sample < dst.caps().samples(); sample++) {
                    auto idata =
                            reinterpret_cast<const SumType *>(sumPacket.constSample(channel,
                                                                                    sample));
                    auto odata =
                            reinterpret_cast<SampleType *>(dst.sample(channel,
                                                                      sample));

                    *odata = qToLittleEndian(scaleValue<SumType,
                                                        SampleType,
                                                        SumType>(*idata,
                                                                 smin,
                                                                 smax));
                }
            }

            return dst;
        }

        template<typename SampleType, typename SumType>
        inline static AkAudioPacket mixChannelsBE(AkAudioCaps::SampleFormat sumFormat,
                                                  AkAudioCaps::ChannelLayout outputLayout,
                                                  const AkAudioPacket &src)
        {
            // Create a summatory packet which type is big enough to contain
            // the sum of all values.
            auto caps = src.caps();
            caps.setFormat(sumFormat);
            caps.setLayout(outputLayout);
            AkAudioPacket sumPacket(caps);
            sumPacket.buffer().fill(0);

            for (int sample = 0; sample < caps.samples(); sample++) {
                for (int ochannel = 0; ochannel < caps.channels(); ochannel++) {
                    auto oposition = sumPacket.caps().position(ochannel);

                    for (int ichannel = 0; ichannel < src.caps().channels(); ichannel++) {
                        /* We use inverse square law to sum the samples
                         * according to the speaker position in the sound dome.
                         *
                         * http://digitalsoundandmusic.com/4-3-4-the-mathematics-of-the-inverse-square-law-and-pag-equations/
                         */
                        auto iposition = src.caps().position(ichannel);
                        auto d = 1.0 + (oposition - iposition);
                        auto k = d * d;

                        auto inSample =
                                reinterpret_cast<const SampleType *>(src.constSample(ichannel,
                                                                                     sample));
                        auto outSample =
                                reinterpret_cast<SumType *>(sumPacket.sample(ochannel,
                                                                             sample));
                        *outSample += SumType(qreal(qFromBigEndian(*inSample)) / k);
                    }
                }
            }

            // Calculate minimum and maximum values of the wave.
            SumType smin;
            SumType smax;
            waveBounds<SumType>(sumPacket, smin, smax);

            caps = src.caps();
            caps.setLayout(outputLayout);
            AkAudioPacket dst(caps);
            dst.copyMetadata(src);

            // Recreate frame with the wave scaled to fit it.
            for (int channel = 0; channel < dst.caps().channels(); channel++) {
                for (int sample = 0; sample < dst.caps().samples(); sample++) {
                    auto idata =
                            reinterpret_cast<const SumType *>(sumPacket.constSample(channel,
                                                                                    sample));
                    auto odata =
                            reinterpret_cast<SampleType *>(dst.sample(channel,
                                                                      sample));

                    *odata = qToBigEndian(scaleValue<SumType,
                                                     SampleType,
                                                     SumType>(*idata,
                                                              smin,
                                                              smax));
                }
            }

            return dst;
        }

#define HANDLE_CASE_CONVERT_LAYOUT(olayout, \
                                   src, \
                                   format, \
                                   sumFormat, \
                                   endian, \
                                   sampleType, \
                                   sumType) \
        case AkAudioCaps::SampleFormat_##format: \
            return mixChannels##endian<sampleType, sumType> \
                    (AkAudioCaps::SampleFormat_##sumFormat, olayout, src);

        inline static AkAudioPacket convertChannels(AkAudioCaps::ChannelLayout outputLayout,
                                                    const AkAudioPacket &src)
        {
            switch (src.caps().format()) {
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s8   , s16,  _, qint8  , qint16 )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u8   , u16,  _, quint8 , quint16)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s16le, s32, LE, qint16 , qint32 )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s16be, s32, BE, qint16 , qint32 )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u16le, u32, LE, quint16, quint32)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u16be, u32, BE, quint16, quint32)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s32le, s64, LE, qint32 , qint64 )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s32be, s64, BE, qint32 , qint64 )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u32le, u64, LE, quint32, quint64)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u32be, u64, BE, quint32, quint64)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s64le, dbl, LE, qint64 , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, s64be, dbl, BE, qint64 , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u64le, dbl, LE, quint64, qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, u64be, dbl, BE, quint64, qreal  )
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, fltle, dbl, LE, float  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, fltbe, dbl, BE, float  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, dblle, dbl, LE, qreal  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, dblbe, dbl, BE, qreal  , qreal  )
#else
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, fltle, dbl,  _, float  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, fltbe, dbl,  _, float  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, dblle, dbl,  _, qreal  , qreal  )
            HANDLE_CASE_CONVERT_LAYOUT(outputLayout, src, dblbe, dbl,  _, qreal  , qreal  )
#endif
            default:
                return {};
            }
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolate_(const AkAudioPacket &packet,
                                              int channel,
                                              qreal isample,
                                              int sample1,
                                              int sample2)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            auto value = (SumType(isample - sample1) * SumType(maxValue - minValue)
                          + SumType(minValue) * SumType(sample2 - sample1))
                         / (sample2 - sample1);

            return SampleType(value);
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolateLE(const AkAudioPacket &packet,
                                               int channel,
                                               qreal isample,
                                               int sample1,
                                               int sample2)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            minValue = qFromLittleEndian(minValue);
            maxValue = qFromLittleEndian(maxValue);
            auto value = (SumType(isample - sample1) * SumType(maxValue - minValue)
                          + SumType(minValue) * SumType(sample2 - sample1))
                         / (sample2 - sample1);

            return SampleType(qToLittleEndian(value));
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolateBE(const AkAudioPacket &packet,
                                               int channel,
                                               qreal isample,
                                               int sample1,
                                               int sample2)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            minValue = qFromBigEndian(minValue);
            maxValue = qFromBigEndian(maxValue);
            auto value = (SumType(isample - sample1) * SumType(maxValue - minValue)
                          + SumType(minValue) * SumType(sample2 - sample1))
                         / (sample2 - sample1);

            return SampleType(qToBigEndian(value));
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolate_(const AkAudioPacket &packet,
                                              int channel,
                                              qreal isample,
                                              int sample1,
                                              int sample2,
                                              int sample3)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto midValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample3));
            auto sample21 = sample1;
            auto sample22 = sample2;
            auto sample23 = sample3;
            /* y = a * x ^ 2 + b * x + c
             *
             * |a|   |x0^2 x0 1|^-1 |y0|;
             * |b| = |x1^2 x1 1|    |y1|;
             * |c|   |x2^2 x2 1|    |y2|;
             */
            auto det = sample21 * SumType(sample2 - sample3) - sample1 * SumType(sample22 - sample23) + SumType(sample22 * sample3 - sample23 * sample2)
                     - sample22 * SumType(sample2 - sample3) + sample2 * SumType(sample21 - sample23) - SumType(sample21 * sample3 - sample23 * sample1)
                     + sample23 * SumType(sample1 - sample2) - sample1 * SumType(sample21 - sample22) + SumType(sample21 * sample2 - sample22 * sample1);
            const SumType matrixValues[] {
                sample2 - sample3, sample23 - sample22, sample22 * sample3 - sample23 * sample2,
                sample3 - sample1, sample21 - sample23, sample23 * sample1 - sample21 * sample3,
                sample1 - sample2, sample22 - sample21, sample21 * sample2 - sample22 * sample1,
            };
            QGenericMatrix<3, 3, SumType> inv(matrixValues);
            const SumType yMatrixValues[] {
                minValue,
                midValue,
                maxValue,
            };
            QGenericMatrix<1, 3, SumType> valuesMatrix(yMatrixValues);
            auto coef = inv * valuesMatrix;
            auto value = (coef(0, 0) * isample * isample + coef(1, 0) * isample + coef(2, 0))
                       / det;

            return SampleType(value);
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolateLE(const AkAudioPacket &packet,
                                               int channel,
                                               qreal isample,
                                               int sample1,
                                               int sample2,
                                               int sample3)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto midValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample3));
            minValue = qFromLittleEndian(minValue);
            midValue = qFromLittleEndian(midValue);
            maxValue = qFromLittleEndian(maxValue);
            auto sample21 = SumType(sample1);
            auto sample22 = SumType(sample2);
            auto sample23 = SumType(sample3);
            /* y = a * x ^ 2 + b * x + c
             *
             * |a|   |x0^2 x0 1|^-1 |y0|;
             * |b| = |x1^2 x1 1|    |y1|;
             * |c|   |x2^2 x2 1|    |y2|;
             */
            auto det = sample21 * SumType(sample2 - sample3) - sample1 * SumType(sample22 - sample23) + SumType(sample22 * sample3 - sample23 * sample2)
                     - sample22 * SumType(sample2 - sample3) + sample2 * SumType(sample21 - sample23) - SumType(sample21 * sample3 - sample23 * sample1)
                     + sample23 * SumType(sample1 - sample2) - sample1 * SumType(sample21 - sample22) + SumType(sample21 * sample2 - sample22 * sample1);
            const SumType matrixValues[] {
                SumType(sample2 - sample3), sample23 - sample22, sample22 * sample3 - sample23 * sample2,
                SumType(sample3 - sample1), sample21 - sample23, sample23 * sample1 - sample21 * sample3,
                SumType(sample1 - sample2), sample22 - sample21, sample21 * sample2 - sample22 * sample1,
            };
            QGenericMatrix<3, 3, SumType> inv(matrixValues);
            const SumType yMatrixValues[] {
                SumType(minValue),
                SumType(midValue),
                SumType(maxValue),
            };
            QGenericMatrix<1, 3, SumType> valuesMatrix(yMatrixValues);
            auto coef = inv * valuesMatrix;
            auto value = (coef(0, 0) * isample * isample + coef(1, 0) * isample + coef(2, 0))
                       / det;

            return SampleType(qToLittleEndian(value));
        }

        template<typename SampleType, typename SumType>
        inline static SampleType interpolateBE(const AkAudioPacket &packet,
                                               int channel,
                                               qreal isample,
                                               int sample1,
                                               int sample2,
                                               int sample3)
        {
            auto minValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample1));
            auto midValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample2));
            auto maxValue = *reinterpret_cast<const SampleType *>(packet.constSample(channel, sample3));
            minValue = qFromBigEndian(minValue);
            midValue = qFromBigEndian(midValue);
            maxValue = qFromBigEndian(maxValue);
            auto sample21 = SumType(sample1);
            auto sample22 = SumType(sample2);
            auto sample23 = SumType(sample3);
            /* y = a * x ^ 2 + b * x + c
             *
             * |a|   |x0^2 x0 1|^-1 |y0|;
             * |b| = |x1^2 x1 1|    |y1|;
             * |c|   |x2^2 x2 1|    |y2|;
             */
            auto det = sample21 * SumType(sample2 - sample3) - sample1 * SumType(sample22 - sample23) + SumType(sample22 * sample3 - sample23 * sample2)
                     - sample22 * SumType(sample2 - sample3) + sample2 * SumType(sample21 - sample23) - SumType(sample21 * sample3 - sample23 * sample1)
                     + sample23 * SumType(sample1 - sample2) - sample1 * SumType(sample21 - sample22) + SumType(sample21 * sample2 - sample22 * sample1);
            const SumType matrixValues[] {
                SumType(sample2 - sample3), sample23 - sample22, sample22 * sample3 - sample23 * sample2,
                SumType(sample3 - sample1), sample21 - sample23, sample23 * sample1 - sample21 * sample3,
                SumType(sample1 - sample2), sample22 - sample21, sample21 * sample2 - sample22 * sample1,
            };
            QGenericMatrix<3, 3, SumType> inv(matrixValues);
            const SumType yMatrixValues[] {
                SumType(minValue),
                SumType(midValue),
                SumType(maxValue),
            };
            QGenericMatrix<1, 3, SumType> valuesMatrix(yMatrixValues);
            auto coef = inv * valuesMatrix;
            auto value = (coef(0, 0) * isample * isample + coef(1, 0) * isample + coef(2, 0))
                       / det;

            return SampleType(qToBigEndian(value));
        }

        using InterpolateLinearFunction =
            std::function<void (const AkAudioPacket &packet,
                                int channel,
                                qreal isample,
                                int sample1,
                                int sample2,
                                quint8 *osample)>;
        using InterpolateQuadraticFunction =
            std::function<void (const AkAudioPacket &packet,
                                int channel,
                                qreal isample,
                                int sample1,
                                int sample2,
                                int sample3,
                                quint8 *osample)>;

#define DEFINE_SAMPLE_INTERPOLATION_FUNCTION(sitype, \
                                             itype, \
                                             optype, \
                                             endian) \
        {AkAudioCaps::SampleFormat_##sitype, \
         [] (const AkAudioPacket &packet, \
             int channel, \
             int isample, \
             int sample1, \
             int sample2, \
             quint8 *osample) { \
            auto value = \
                interpolate##endian<itype, optype> \
                    (packet, channel, isample, sample1, sample2); \
            memcpy(osample, &value, sizeof(itype)); \
         }, \
         [] (const AkAudioPacket &packet, \
             int channel, \
             int isample, \
             int sample1, \
             int sample2, \
             int sample3, \
             quint8 *osample) { \
            auto value = \
                interpolate##endian<itype, optype> \
                    (packet, channel, isample, sample1, sample2, sample3); \
            memcpy(osample, &value, sizeof(itype)); \
         }}

        struct AudioSamplesInterpolation
        {
            AkAudioCaps::SampleFormat format;
            InterpolateLinearFunction linear;
            InterpolateQuadraticFunction quadratic;
        };

        using AudioSamplesInterpolationFuncs = QVector<AudioSamplesInterpolation>;

        inline static const AudioSamplesInterpolationFuncs &samplesInterpolation()
        {
            static const AudioSamplesInterpolationFuncs interpolation {
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s8   ,   qint8, qint64,  _),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u8   ,  quint8, qint64,  _),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s16le,  qint16, qint64, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s16be,  qint16, qint64, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u16le, quint16, qint64, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u16be, quint16, qint64, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s32le,  qint32, qint64, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s32be,  qint32, qint64, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u32le, quint32, qint64, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u32be, quint32, qint64, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s64le,  qint64,  qreal, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(s64be,  qint64,  qreal, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u64le, quint64,  qreal, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(u64be, quint64,  qreal, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(fltle,   float,  qreal, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(fltbe,   float,  qreal, BE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(dblle,   qreal,  qreal, LE),
                DEFINE_SAMPLE_INTERPOLATION_FUNCTION(dblbe,   qreal,  qreal, BE),
            };

            return interpolation;
        }

        inline static const AudioSamplesInterpolation *bySamplesInterpolationFormat(AkAudioCaps::SampleFormat format)
        {
            for (auto &interpolation: samplesInterpolation())
                if (interpolation.format == format)
                    return &interpolation;

            return &samplesInterpolation().front();
        }
};

AkAudioPacket::AkAudioPacket(QObject *parent):
    QObject(parent)
{
    this->d = new AkAudioPacketPrivate();
}

AkAudioPacket::AkAudioPacket(const AkAudioCaps &caps)
{
    this->d = new AkAudioPacketPrivate();
    this->d->m_caps = caps;
    this->d->m_buffer = QByteArray(int(caps.frameSize()), Qt::Uninitialized);
}

AkAudioPacket::AkAudioPacket(const AkPacket &other)
{
    this->d = new AkAudioPacketPrivate();
    this->d->m_caps = other.caps();
    this->d->m_buffer = other.buffer();
    this->d->m_pts = other.pts();
    this->d->m_timeBase = other.timeBase();
    this->d->m_index = other.index();
    this->d->m_id = other.id();
}

AkAudioPacket::AkAudioPacket(const AkAudioPacket &other):
    QObject()
{
    this->d = new AkAudioPacketPrivate();
    this->d->m_caps = other.d->m_caps;
    this->d->m_buffer = other.d->m_buffer;
    this->d->m_pts = other.d->m_pts;
    this->d->m_timeBase = other.d->m_timeBase;
    this->d->m_index = other.d->m_index;
    this->d->m_id = other.d->m_id;
}

AkAudioPacket::~AkAudioPacket()
{
    delete this->d;
}

AkAudioPacket &AkAudioPacket::operator =(const AkPacket &other)
{
    this->d->m_caps = other.caps();
    this->d->m_buffer = other.buffer();
    this->d->m_pts = other.pts();
    this->d->m_timeBase = other.timeBase();
    this->d->m_index = other.index();
    this->d->m_id = other.id();

    return *this;
}

AkAudioPacket &AkAudioPacket::operator =(const AkAudioPacket &other)
{
    if (this != &other) {
        this->d->m_caps = other.d->m_caps;
        this->d->m_buffer = other.d->m_buffer;
        this->d->m_pts = other.d->m_pts;
        this->d->m_timeBase = other.d->m_timeBase;
        this->d->m_index = other.d->m_index;
        this->d->m_id = other.d->m_id;
    }

    return *this;
}

AkAudioPacket AkAudioPacket::operator +(const AkAudioPacket &other)
{
    auto tmpPacket = other.convert(this->caps());

    if (!tmpPacket)
        return *this;

    auto caps = this->caps();
    caps.setSamples(this->caps().samples() + tmpPacket.caps().samples());
    AkAudioPacket packet(caps);
    packet.copyMetadata(other);

    for (int plane = 0; plane < caps.planes(); plane++) {
        auto start = this->caps().bytesPerPlane();
        memcpy(packet.planeData(plane),
               this->constPlaneData(plane),
               start);
        memcpy(packet.planeData(plane) + start,
               other.constPlaneData(plane),
               other.caps().bytesPerPlane());
    }

    return packet;
}

AkAudioPacket &AkAudioPacket::operator +=(const AkAudioPacket &other)
{
    auto tmpPacket = other.convert(this->caps());

    if (!tmpPacket)
        return *this;

    auto caps = this->caps();
    caps.setSamples(this->caps().samples() + tmpPacket.caps().samples());
    AkAudioPacket packet(caps);
    packet.copyMetadata(other);

    for (int plane = 0; plane < caps.planes(); plane++) {
        auto start = this->caps().bytesPerPlane();
        memcpy(packet.planeData(plane),
               this->constPlaneData(plane),
               start);
        memcpy(packet.planeData(plane) + start,
               other.constPlaneData(plane),
               other.caps().bytesPerPlane());
    }

    *this = packet;

    return *this;
}

AkAudioPacket::operator AkPacket() const
{
    AkPacket packet(this->d->m_caps);
    packet.buffer() = this->d->m_buffer;
    packet.pts() = this->d->m_pts;
    packet.timeBase() = this->d->m_timeBase;
    packet.index() = this->d->m_index;
    packet.id() = this->d->m_id;

    return packet;
}

AkAudioPacket::operator bool() const
{
    return this->d->m_caps;
}

AkAudioCaps AkAudioPacket::caps() const
{
    return this->d->m_caps;
}

AkAudioCaps &AkAudioPacket::caps()
{
    return this->d->m_caps;
}

QByteArray AkAudioPacket::buffer() const
{
    return this->d->m_buffer;
}

QByteArray &AkAudioPacket::buffer()
{
    return this->d->m_buffer;
}

qint64 AkAudioPacket::id() const
{
    return this->d->m_id;
}

qint64 &AkAudioPacket::id()
{
    return this->d->m_id;
}

qint64 AkAudioPacket::pts() const
{
    return this->d->m_pts;
}

qint64 &AkAudioPacket::pts()
{
    return this->d->m_pts;
}

AkFrac AkAudioPacket::timeBase() const
{
    return this->d->m_timeBase;
}

AkFrac &AkAudioPacket::timeBase()
{
    return this->d->m_timeBase;
}

int AkAudioPacket::index() const
{
    return this->d->m_index;
}

int &AkAudioPacket::index()
{
    return this->d->m_index;
}

void AkAudioPacket::copyMetadata(const AkAudioPacket &other)
{
    this->d->m_pts = other.d->m_pts;
    this->d->m_timeBase = other.d->m_timeBase;
    this->d->m_index = other.d->m_index;
    this->d->m_id = other.d->m_id;
}

const quint8 *AkAudioPacket::constPlaneData(int plane) const
{
    return reinterpret_cast<const quint8 *>(this->d->m_buffer.constData())
            + this->d->m_caps.planeOffset(plane);
}

quint8 *AkAudioPacket::planeData(int plane)
{
    return reinterpret_cast<quint8 *>(this->d->m_buffer.data())
            + this->d->m_caps.planeOffset(plane);
}

const quint8 *AkAudioPacket::constSample(int channel, int i) const
{
    auto bps = this->d->m_caps.bps();

    if (this->d->m_caps.planar())
        return this->constPlaneData(channel) + i * bps / 8;

    auto channels = this->d->m_caps.channels();

    return this->constPlaneData(0) + (i * channels + channel) * bps / 8;
}

quint8 *AkAudioPacket::sample(int channel, int i)
{
    auto bps = this->d->m_caps.bps();

    if (this->d->m_caps.planar())
        return this->planeData(channel) + i * bps / 8;

    auto channels = this->d->m_caps.channels();

    return this->planeData(0) + (i * channels + channel) * bps / 8;
}

void AkAudioPacket::setSample(int channel, int i, const quint8 *sample)
{
    memcpy(this->sample(channel, i), sample, size_t(this->d->m_caps.bps()) / 8);
}

AkAudioPacket AkAudioPacket::convert(const AkAudioCaps &caps) const
{
    auto packet = this->convertFormat(caps.format());

    if (!packet)
        return {};

    packet = packet.convertLayout(caps.layout());

    if (!packet)
        return {};

    return packet.convertPlanar(caps.planar());
}

bool AkAudioPacket::canConvertFormat(AkAudioCaps::SampleFormat input,
                               AkAudioCaps::SampleFormat output)
{
    if (input == output)
        return true;

    bool fromFormat = false;
    bool toFormat = false;

    for (auto &convert: AkAudioPacketPrivate::sampleFormatConvert()) {
        if (convert.from == input)
            fromFormat = true;

        if (convert.to == output)
            toFormat = true;

        if (fromFormat && toFormat)
            return true;
    }

    return false;
}

bool AkAudioPacket::canConvertFormat(AkAudioCaps::SampleFormat output) const
{
    return AkAudioPacket::canConvertFormat(this->d->m_caps.format(), output);
}

AkAudioPacket AkAudioPacket::convertFormat(AkAudioCaps::SampleFormat format) const
{
    if (this->d->m_caps.format() == format)
        return *this;

    AudioConvertFuntion convertFrom;
    AudioConvertFuntion convertTo;

    for (auto &convert: AkAudioPacketPrivate::sampleFormatConvert()) {
        if (convert.from == this->d->m_caps.format())
            convertFrom = convert.convert;

        if (convert.to == format)
            convertTo = convert.convert;

        if (convert.from == this->d->m_caps.format()
            && convert.to == format) {
            return convert.convert(*this);
        }
    }

    if (convertFrom && convertTo)
        return convertTo(convertFrom(*this));

    return {};
}

AkAudioPacket AkAudioPacket::convertLayout(AkAudioCaps::ChannelLayout layout) const
{
    if (this->d->m_caps.layout() == layout)
        return *this;

    return AkAudioPacketPrivate::convertChannels(layout, *this);
}

AkAudioPacket AkAudioPacket::convertSampleRate(int rate,
                                               qreal &sampleCorrection,
                                               ResampleMethod method) const
{
    if (rate == this->d->m_caps.rate())
        return *this;

    auto rSamples = qreal(this->d->m_caps.samples())
                    * rate
                    / this->d->m_caps.rate()
                    + sampleCorrection;
    auto oSamples = qRound(rSamples);

    if (oSamples < 1)
        return {};

    auto caps = this->d->m_caps;
    caps.setSamples(oSamples);
    caps.setRate(rate);
    AkAudioPacket packet(caps);

    if (oSamples <  this->d->m_caps.samples())
        method = ResampleMethod_Fast;

    switch (method) {
    case ResampleMethod_Fast:
        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = sample
                               * (this->d->m_caps.samples() - 1)
                               / (oSamples - 1);
                auto iValue = this->constSample(channel, iSample);
                packet.setSample(channel, sample, iValue);
            }
        }

        break;

    case ResampleMethod_Linear: {
        auto sif =
                AkAudioPacketPrivate::bySamplesInterpolationFormat(caps.format());
        auto interpolation = sif->linear;

        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = qreal(sample)
                               * (this->d->m_caps.samples() - 1)
                               / (oSamples - 1);
                auto minSample = qFloor(iSample);
                auto maxSample = qCeil(iSample);

                if (minSample == maxSample) {
                    auto iValue = this->constSample(channel, minSample);
                    packet.setSample(channel, sample, iValue);
                } else {
                    quint64 data;
                    interpolation(*this,
                                  channel,
                                  iSample,
                                  minSample,
                                  maxSample,
                                  reinterpret_cast<quint8 *>(&data));
                    packet.setSample(channel,
                                     sample,
                                     reinterpret_cast<const quint8 *>(&data));
                }
            }
        }

        break;
    }

    case ResampleMethod_Quadratic:
        auto sif =
                AkAudioPacketPrivate::bySamplesInterpolationFormat(caps.format());
        auto interpolationL = sif->linear;
        auto interpolationQ = sif->quadratic;

        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = qreal(sample)
                               * (this->d->m_caps.samples() - 1)
                               / (oSamples - 1);
                auto minSample = qFloor(iSample);
                auto maxSample = qCeil(iSample);

                if (minSample == maxSample) {
                    auto iValue = this->constSample(channel, minSample);
                    packet.setSample(channel, sample, iValue);
                } else {
                    auto diffMinSample = minSample - iSample;
                    auto diffMaxSample = maxSample - iSample;
                    diffMinSample *= diffMinSample;
                    diffMaxSample *= diffMaxSample;
                    auto midSample = diffMinSample < diffMaxSample?
                                         qMax(minSample - 1, 0):
                                         qMin(maxSample + 1, this->d->m_caps.samples() - 1);

                    if (midSample < minSample)
                        std::swap(midSample, minSample);

                    if (midSample > maxSample)
                        std::swap(midSample, maxSample);

                    if (midSample == minSample
                        || midSample == maxSample) {
                        quint64 data;
                        interpolationL(*this,
                                       channel,
                                       iSample,
                                       minSample,
                                       maxSample,
                                       reinterpret_cast<quint8 *>(&data));
                        packet.setSample(channel,
                                         sample,
                                         reinterpret_cast<const quint8 *>(&data));
                    } else {
                        quint64 data;
                        interpolationQ(*this,
                                       channel,
                                       iSample,
                                       minSample,
                                       midSample,
                                       maxSample,
                                       reinterpret_cast<quint8 *>(&data));
                        packet.setSample(channel,
                                         sample,
                                         reinterpret_cast<const quint8 *>(&data));
                    }
                }
            }
        }

        break;
    }

    sampleCorrection = rSamples - oSamples;

    return packet;
}

AkAudioPacket AkAudioPacket::scale(int samples,
                                   AkAudioPacket::ResampleMethod method) const
{
    if (samples == this->d->m_caps.samples())
        return *this;

    if (samples < 1)
        return {};

    auto caps = this->d->m_caps;
    caps.setSamples(samples);
    AkAudioPacket packet(caps);

    if (samples <  this->d->m_caps.samples())
        method = ResampleMethod_Fast;

    switch (method) {
    case ResampleMethod_Fast:
        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = sample
                               * (this->d->m_caps.samples() - 1)
                               / (samples - 1);
                auto iValue = this->constSample(channel, iSample);
                packet.setSample(channel, sample, iValue);
            }
        }

        break;

    case ResampleMethod_Linear: {
        auto sif =
                AkAudioPacketPrivate::bySamplesInterpolationFormat(caps.format());
        auto interpolation = sif->linear;

        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = qreal(sample)
                               * (this->d->m_caps.samples() - 1)
                               / (samples - 1);
                auto minSample = qFloor(iSample);
                auto maxSample = qCeil(iSample);

                if (minSample == maxSample) {
                    auto iValue = this->constSample(channel, minSample);
                    packet.setSample(channel, sample, iValue);
                } else {
                    quint64 data;
                    interpolation(*this,
                                  channel,
                                  iSample,
                                  minSample,
                                  maxSample,
                                  reinterpret_cast<quint8 *>(&data));
                    packet.setSample(channel,
                                     sample,
                                     reinterpret_cast<const quint8 *>(&data));
                }
            }
        }

        break;
    }

    case ResampleMethod_Quadratic:
        auto sif =
                AkAudioPacketPrivate::bySamplesInterpolationFormat(caps.format());
        auto interpolationL = sif->linear;
        auto interpolationQ = sif->quadratic;

        for (int channel = 0; channel < packet.caps().channels(); channel++) {
            for (int sample = 0; sample < packet.caps().samples(); sample++) {
                auto iSample = qreal(sample)
                               * (this->d->m_caps.samples() - 1)
                               / (samples - 1);
                auto minSample = qFloor(iSample);
                auto maxSample = qCeil(iSample);

                if (minSample == maxSample) {
                    auto iValue = this->constSample(channel, minSample);
                    packet.setSample(channel, sample, iValue);
                } else {
                    auto diffMinSample = minSample - iSample;
                    auto diffMaxSample = maxSample - iSample;
                    diffMinSample *= diffMinSample;
                    diffMaxSample *= diffMaxSample;
                    auto midSample = diffMinSample < diffMaxSample?
                                         qMax(minSample - 1, 0):
                                         qMin(maxSample + 1, this->d->m_caps.samples() - 1);

                    if (midSample < minSample)
                        std::swap(midSample, minSample);

                    if (midSample > maxSample)
                        std::swap(midSample, maxSample);

                    if (midSample == minSample
                        || midSample == maxSample) {
                        quint64 data;
                        interpolationL(*this,
                                       channel,
                                       iSample,
                                       minSample,
                                       maxSample,
                                       reinterpret_cast<quint8 *>(&data));
                        packet.setSample(channel,
                                         sample,
                                         reinterpret_cast<const quint8 *>(&data));
                    } else {
                        quint64 data;
                        interpolationQ(*this,
                                       channel,
                                       iSample,
                                       minSample,
                                       midSample,
                                       maxSample,
                                       reinterpret_cast<quint8 *>(&data));
                        packet.setSample(channel,
                                         sample,
                                         reinterpret_cast<const quint8 *>(&data));
                    }
                }
            }
        }

        break;
    }

    return packet;
}

AkAudioPacket AkAudioPacket::convertPlanar(bool planar) const
{
    if ((this->d->m_caps.planar()) == planar)
        return *this;

    auto caps = this->d->m_caps;
    caps.updatePlaneSize(planar);
    AkAudioPacket dst(caps);
    dst.copyMetadata(*this);
    auto byps = caps.bps() / 8;
    auto channels = caps.channels();

    if (planar) {
        auto src_line = reinterpret_cast<const qint8 *>(this->constPlaneData(0));

        for (int plane = 0; plane < caps.planes(); plane++) {
            auto dst_line = reinterpret_cast<qint8 *>(dst.planeData(plane));

            for (int i = 0; i < caps.samples(); i++)
                memcpy(dst_line + byps * i,
                       src_line + byps * (i * channels + plane),
                       size_t(byps));
        }
    } else {
        auto dst_line = reinterpret_cast<qint8 *>(dst.planeData(0));

        for (int plane = 0; plane < caps.planes(); plane++) {
            auto src_line = reinterpret_cast<const qint8 *>(this->constPlaneData(plane));

            for (int i = 0; i < caps.samples(); i++)
                memcpy(dst_line + byps * (i * channels + plane),
                       src_line + byps * i,
                       size_t(byps));
        }
    }

    return dst;
}

AkAudioPacket AkAudioPacket::realign(int align) const
{
    auto caps = this->d->m_caps;
    caps.realign(align);

    if (caps == this->d->m_caps)
        return *this;

    AkAudioPacket dst(caps);
    dst.copyMetadata(*this);

    for (int plane = 0; plane < caps.planes(); plane++) {
        auto planeSize = qMin(caps.planeSize()[plane],
                              this->d->m_caps.planeSize()[plane]);
        auto src_line = this->constPlaneData(plane);
        auto dst_line = dst.planeData(plane);
        memcpy(dst_line, src_line, planeSize);
    }

    return dst;
}

AkAudioPacket AkAudioPacket::pop(int samples)
{
    auto caps = this->d->m_caps;
    samples = qMin(caps.samples(), samples);

    if (samples < 1)
        return {};

    caps.setSamples(samples);
    AkAudioPacket dst(caps);
    dst.copyMetadata(*this);

    caps.setSamples(this->d->m_caps.samples() - samples);
    AkAudioPacket tmpPacket(caps);
    tmpPacket.copyMetadata(*this);

    for (int plane = 0; plane < dst.caps().planes(); plane++) {
        auto src_line = this->constPlaneData(plane);
        auto dst_line = dst.planeData(plane);
        auto dataSize = dst.caps().planeSize()[plane];
        memcpy(dst_line, src_line, dataSize);

        src_line = this->constPlaneData(plane) + dataSize;
        dst_line = tmpPacket.planeData(plane);
        dataSize = tmpPacket.caps().planeSize()[plane];

        if (dataSize > 0)
            memcpy(dst_line, src_line, dataSize);
    }

    *this = tmpPacket;

    return dst;
}

void AkAudioPacket::setCaps(const AkAudioCaps &caps)
{
    if (this->d->m_caps == caps)
        return;

    this->d->m_caps = caps;
    emit this->capsChanged(caps);
}

void AkAudioPacket::setBuffer(const QByteArray &buffer)
{
    if (this->d->m_buffer == buffer)
        return;

    this->d->m_buffer = buffer;
    emit this->bufferChanged(buffer);
}

void AkAudioPacket::setId(qint64 id)
{
    if (this->d->m_id == id)
        return;

    this->d->m_id = id;
    emit this->idChanged(id);
}

void AkAudioPacket::setPts(qint64 pts)
{
    if (this->d->m_pts == pts)
        return;

    this->d->m_pts = pts;
    emit this->ptsChanged(pts);
}

void AkAudioPacket::setTimeBase(const AkFrac &timeBase)
{
    if (this->d->m_timeBase == timeBase)
        return;

    this->d->m_timeBase = timeBase;
    emit this->timeBaseChanged(timeBase);
}

void AkAudioPacket::setIndex(int index)
{
    if (this->d->m_index == index)
        return;

    this->d->m_index = index;
    emit this->indexChanged(index);
}

void AkAudioPacket::resetCaps()
{
    this->setCaps(AkAudioCaps());
}

void AkAudioPacket::resetBuffer()
{
    this->setBuffer({});
}

void AkAudioPacket::resetId()
{
    this->setId(-1);
}

void AkAudioPacket::resetPts()
{
    this->setPts(0);
}

void AkAudioPacket::resetTimeBase()
{
    this->setTimeBase({});
}

void AkAudioPacket::resetIndex()
{
    this->setIndex(-1);
}

QDebug operator <<(QDebug debug, const AkAudioPacket &packet)
{
    debug.nospace() << "AkAudioPacket("
                    << "caps="
                    << packet.caps()
                    << ",bufferSize="
                    << packet.buffer().size()
                    << ",id="
                    << packet.id()
                    << ",pts="
                    << packet.pts()
                    << "("
                    << packet.pts() * packet.timeBase().value()
                    << ")"
                    << ",timeBase="
                    << packet.timeBase()
                    << ",index="
                    << packet.index()
                    << ")";

    return debug.space();
}

QDebug operator <<(QDebug debug, AkAudioPacket::ResampleMethod method)
{
    AkAudioPacket packet;
    int resampleMethodIndex = packet.metaObject()->indexOfEnumerator("ResampleMethod");
    QMetaEnum resampleMethodEnum = packet.metaObject()->enumerator(resampleMethodIndex);
    QString resampleMethodStr(resampleMethodEnum.valueToKey(method));
    resampleMethodStr.remove("ResampleMethod_");
    debug.nospace() << resampleMethodStr.toStdString().c_str();

    return debug.space();
}

#include "moc_akaudiopacket.cpp"
