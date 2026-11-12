/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2026 Q2H2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <extdef.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>

#include "analogsignal.hpp"
#include "logicsignal.hpp"
#include "view.hpp"
#include "viewport.hpp"

#include "pv/util.hpp"
#include "pv/data/analog.hpp"
#include "pv/data/analogsegment.hpp"
#include "pv/data/logic.hpp"
#include "pv/data/logicsegment.hpp"
#include "pv/data/signalbase.hpp"
#include "pv/devicemanager.hpp"
#include "pv/globalsettings.hpp"
#include <pv/session.hpp>

#include <libsigrokcxx/libsigrokcxx.hpp>

using std::deque;
using std::div;
using std::div_t;
// Note that "using std::isnan;" is _not_ put here since that would break
// compilation on some platforms. Use "std::isnan()" instead in checks below.
using std::make_pair;
using std::max;
using std::min;
using std::none_of;
using std::numeric_limits;
using std::out_of_range;
using std::pair;
using std::shared_ptr;
using std::vector;

using pv::data::LogicSegment;
using pv::data::SignalBase;
using pv::util::determine_value_prefix;
using pv::util::SIPrefix;

using sigrok::TriggerMatch;
using sigrok::TriggerMatchType;

namespace pv
{
	namespace views
	{
		namespace trace
		{

			const QPen AnalogSignal::AxisPen(QColor(0, 0, 0, 30 * 256 / 100), 2);
			const QColor AnalogSignal::GridMajorColor = QColor(0, 0, 0, 40 * 256 / 100);
			const QColor AnalogSignal::GridMinorColor = QColor(0, 0, 0, 20 * 256 / 100);

			const QColor AnalogSignal::SamplingPointColorLo = QColor(200, 0, 0, 80 * 256 / 100);
			const QColor AnalogSignal::SamplingPointColorNe = QColor(0, 0, 0, 80 * 256 / 100);
			const QColor AnalogSignal::SamplingPointColorHi = QColor(0, 200, 0, 80 * 256 / 100);

			const QColor AnalogSignal::ThresholdColor = QColor(0, 0, 0, 30 * 256 / 100);
			const QColor AnalogSignal::ThresholdColorLo = QColor(255, 0, 0, 8 * 256 / 100);
			const QColor AnalogSignal::ThresholdColorNe = QColor(0, 0, 0, 10 * 256 / 100);
			const QColor AnalogSignal::ThresholdColorHi = QColor(0, 255, 0, 8 * 256 / 100);

			const int64_t AnalogSignal::TracePaintBlockSize = 1024 * 1024; // 4 MiB (due to float)
			const float AnalogSignal::EnvelopeThreshold = 64.0f;

			const int AnalogSignal::MaximumVDivs = 10;
			const int AnalogSignal::MinScaleIndex = -6; // 0.01 units/div
			const int AnalogSignal::MaxScaleIndex = 10; // 1000 units/div

			const int AnalogSignal::InfoTextMarginRight = 20;
			const int AnalogSignal::InfoTextMarginBottom = 5;

			AnalogSignal::AnalogSignal(pv::Session &session, shared_ptr<data::SignalBase> base) : LogicSignal(session, base),
																								  value_at_hover_pos_(std::numeric_limits<float>::quiet_NaN()),
																								  scale_index_(0), // 1 per div
																								  pos_vdivs_(5),
																								  neg_vdivs_(0),
																								  resolution_(0),
																								  display_type_(DisplayAnalog),
																								  autoranging_(false),
																								  sine_interpolation_(false)
			{
				axis_pen_ = AxisPen;

				pv::data::Analog *analog_data =
					dynamic_cast<pv::data::Analog *>(base_->analog_data().get());

				connect(analog_data, SIGNAL(min_max_changed(float, float)),
						this, SLOT(on_min_max_changed(float, float)));

				GlobalSettings settings;
				show_sampling_points_ =
					settings.value(GlobalSettings::Key_View_ShowSamplingPoints).toBool();
				fill_high_areas_ =
					settings.value(GlobalSettings::Key_View_FillSignalHighAreas).toBool();
				high_fill_color_ = QColor::fromRgba(settings.value(
																GlobalSettings::Key_View_FillSignalHighAreaColor)
														.value<uint32_t>());
				show_analog_minor_grid_ =
					settings.value(GlobalSettings::Key_View_ShowAnalogMinorGrid).toBool();
				conversion_threshold_disp_mode_ =
					settings.value(GlobalSettings::Key_View_ConversionThresholdDispMode).toInt();
				div_height_ = 120;

				update_logic_level_offsets();
				update_scale();
				show_hover_marker_ = true;

				/* ADC电平触发：从当前session trigger同步门限值(触发类型trigger_match_
				 * 已由LogicSignal构造函数同步)。无触发时为NAN。 */
				trigger_type_cb_ = nullptr;
				trigger_level_sb_ = nullptr;
				analog_trigger_value_ = std::numeric_limits<float>::quiet_NaN();
				if (auto trigger = session_.session()->trigger())
					for (auto stage : trigger->stages())
						for (auto match : stage->matches())
							if (match->channel() == base_->channel())
								analog_trigger_value_ = match->value();
			}

			std::map<QString, QVariant> AnalogSignal::save_settings() const
			{
				LogicSignal::save_settings();

				std::map<QString, QVariant> result;

				result["pos_vdivs"] = pos_vdivs_;
				result["neg_vdivs"] = neg_vdivs_;
				result["scale_index"] = scale_index_;
				result["display_type"] = display_type_;
				result["autoranging"] = autoranging_;
				result["sine_interpolation"] = sine_interpolation_;
				result["div_height"] = div_height_;

				/* ADC电平触发 */
				if (trigger_match_)
					result["analog_trigger_type"] = (int)trigger_match_->id();
				if (!std::isnan(analog_trigger_value_))
					result["analog_trigger_value"] = (double)analog_trigger_value_;

				return result;
			}

			void AnalogSignal::restore_settings(std::map<QString, QVariant> settings)
			{
				LogicSignal::restore_settings(settings);

				auto entry = settings.find("pos_vdivs");
				if (entry != settings.end())
					pos_vdivs_ = settings["pos_vdivs"].toInt();

				entry = settings.find("neg_vdivs");
				if (entry != settings.end())
					neg_vdivs_ = settings["neg_vdivs"].toInt();

				entry = settings.find("scale_index");
				if (entry != settings.end())
				{
					scale_index_ = settings["scale_index"].toInt();
					update_scale();
				}

				entry = settings.find("display_type");
				if (entry != settings.end())
					display_type_ = (DisplayType)(settings["display_type"].toInt());

				entry = settings.find("autoranging");
				if (entry != settings.end())
					autoranging_ = settings["autoranging"].toBool();

				entry = settings.find("sine_interpolation");
				if (entry != settings.end())
					sine_interpolation_ = settings["sine_interpolation"].toBool();

				entry = settings.find("div_height");
				if (entry != settings.end())
				{
					const int old_height = div_height_;
					div_height_ = settings["div_height"].toInt();

					update_logic_level_offsets();

					if ((div_height_ != old_height) && owner_)
					{
						// Call order is important, otherwise the lazy event handler won't work
						owner_->extents_changed(false, true);
						owner_->row_item_appearance_changed(false, true);
					}
				}

				/* ADC电平触发 */
				bool trig_changed = false;
				entry = settings.find("analog_trigger_type");
				if (entry != settings.end())
				{
					int type_id = settings["analog_trigger_type"].toInt();
					if (type_id == 0)
						trigger_match_ = nullptr;
					else
						trigger_match_ = TriggerMatchType::get(type_id);
					trig_changed = true;
				}

				entry = settings.find("analog_trigger_value");
				if (entry != settings.end())
				{
					analog_trigger_value_ = settings["analog_trigger_value"].toFloat();
					trig_changed = true;
				}

				/* 仅有类型无门限时给个默认值,避免 NAN 门限导致触发永不命中 */
				if (trigger_match_ && std::isnan(analog_trigger_value_))
					analog_trigger_value_ = 1.65f;

				if (trig_changed)
					modify_trigger();
			}

			pair<int, int> AnalogSignal::v_extents() const
			{
				const int ph = pos_vdivs_ * div_height_;
				const int nh = neg_vdivs_ * div_height_;

				return make_pair(-ph, nh);
			}

			void AnalogSignal::paint_back(QPainter &p, ViewItemPaintParams &pp)
			{
				if (!base_->enabled())
					return;

				bool paint_thr_bg =
					conversion_threshold_disp_mode_ == GlobalSettings::ConvThrDispMode_Background;

				const vector<double> thresholds = base_->get_conversion_thresholds();

				// Only display thresholds if we have some and we show analog samples
				if ((thresholds.size() > 0) && paint_thr_bg &&
					((display_type_ == DisplayAnalog) || (display_type_ == DisplayBoth)))
				{

					const int visual_y = get_visual_y();
					const pair<int, int> extents = v_extents();
					const int top = visual_y + extents.first;
					const int btm = visual_y + extents.second;

					// Draw high/neutral/low areas
					if (thresholds.size() == 2)
					{
						int thr_lo = visual_y - thresholds[0] * scale_;
						int thr_hi = visual_y - thresholds[1] * scale_;
						thr_lo = min(max(thr_lo, top), btm);
						thr_hi = min(max(thr_hi, top), btm);

						p.fillRect(QRectF(pp.left(), top, pp.width(), thr_hi - top),
								   QBrush(ThresholdColorHi));
						p.fillRect(QRectF(pp.left(), thr_hi, pp.width(), thr_lo - thr_hi),
								   QBrush(ThresholdColorNe));
						p.fillRect(QRectF(pp.left(), thr_lo, pp.width(), btm - thr_lo),
								   QBrush(ThresholdColorLo));
					}
					else
					{
						int thr = visual_y - thresholds[0] * scale_;
						thr = min(max(thr, top), btm);

						p.fillRect(QRectF(pp.left(), top, pp.width(), thr - top),
								   QBrush(ThresholdColorHi));
						p.fillRect(QRectF(pp.left(), thr, pp.width(), btm - thr),
								   QBrush(ThresholdColorLo));
					}

					paint_axis(p, pp, get_visual_y());
				}
				else
				{
					Signal::paint_back(p, pp);
					paint_axis(p, pp, get_visual_y());
				}
			}

			void AnalogSignal::paint_mid(QPainter &p, ViewItemPaintParams &pp)
			{
				assert(base_->analog_data());
				assert(owner_);

				const int y = get_visual_y();

				if (!base_->enabled())
					return;

				if ((display_type_ == DisplayAnalog) || (display_type_ == DisplayBoth))
				{
					paint_grid(p, y, pp.left(), pp.right());

					shared_ptr<pv::data::AnalogSegment> segment = get_analog_segment_to_paint();

					if (segment && (segment->get_sample_count() > 0))
					{
						const double pixels_offset = pp.pixels_offset();
						const double samplerate = max(1.0, segment->samplerate());
						const pv::util::Timestamp &start_time = segment->start_time();
						const int64_t last_sample = (int64_t)segment->get_sample_count() - 1;
						const double samples_per_pixel = samplerate * pp.scale();
						const pv::util::Timestamp start = samplerate * (pp.offset() - start_time);
						const pv::util::Timestamp end = start + samples_per_pixel * pp.width();

						const int64_t start_sample = min(max(floor(start).convert_to<int64_t>(),
															 (int64_t)0),
														 last_sample);
						const int64_t end_sample = min(max((ceil(end) + 1).convert_to<int64_t>(),
														   (int64_t)0),
													   last_sample);

						if (samples_per_pixel < EnvelopeThreshold)
							paint_trace(p, segment, y, pp.left(), start_sample, end_sample,
										pixels_offset, samples_per_pixel);
						else
							paint_envelope(p, segment, y, pp.left(), start_sample, end_sample,
										   pixels_offset, samples_per_pixel);
					}
				}

				if ((display_type_ == DisplayConverted) || (display_type_ == DisplayBoth))
					if (base_->logic_data())
						LogicSignal::paint_mid(p, pp);

				const QString err = base_->get_error_message();
				if (!err.isEmpty())
					paint_error(p, pp);
			}

			void AnalogSignal::paint_fore(QPainter &p, ViewItemPaintParams &pp)
			{
				if (!enabled())
					return;

				if ((display_type_ == DisplayAnalog) || (display_type_ == DisplayBoth))
				{
					const int y = get_visual_y();

					QString infotext;

					// Show the info section on the right side of the trace, including
					// the value at the hover point when the hover marker is enabled
					// and we have corresponding data available
					if (show_hover_marker_ && !std::isnan(value_at_hover_pos_))
					{
						// Use the hover value itself to determine appropriate SI prefix
						SIPrefix hover_prefix = determine_value_prefix(fabs(value_at_hover_pos_));
						infotext = QString("[%1] %2 V/div")
									   .arg(format_value_si(value_at_hover_pos_, hover_prefix, 3, "V", false))
									   .arg(resolution_);
					}
					else
					{
						SIPrefix prefix;
						if (fabs(signal_max_) > fabs(signal_min_))
							prefix = determine_value_prefix(fabs(signal_max_));
						else
							prefix = determine_value_prefix(fabs(signal_min_));
						infotext = QString("%1 V/div").arg(resolution_);
					}

					p.setPen(base_->color());
					p.setFont(QApplication::font());

					const QRectF bounding_rect = QRectF(pp.left(),
														y + v_extents().first,
														pp.width() - InfoTextMarginRight,
														v_extents().second - v_extents().first - InfoTextMarginBottom);

					p.drawText(bounding_rect, Qt::AlignRight | Qt::AlignBottom, infotext);

					/* ADC电平触发门限标注：采用与逻辑分析仪相同的触发标记样式
					 * (黄色圆角背景 + 触发类型图标),并附带电压门限值。置于右侧,
					 * 垂直对齐到触发门限电平;不画整条横跨虚线,不支持鼠标拖动。 */
					if (trigger_match_ && !std::isnan(analog_trigger_value_))
					{
						const pair<int, int> extents = v_extents();
						const int top = y + extents.first;
						const int btm = y + extents.second;
						int y_trig = y - (int)(analog_trigger_value_ * scale_);
						y_trig = std::min(std::max(y_trig, top), btm);

						const float pad = TriggerMarkerPadding - 0.5f;

						/* 电压门限文本 */
						const QString trig_text =
							QString::number(analog_trigger_value_, 'f', 2) + "V";
						const QFontMetrics fm(p.font());
						const int text_w = fm.boundingRect(trig_text).width();
						const int text_h = fm.height();

						/* 触发类型图标(Over/Under 无内置图标,仅显示文本) */
						const int type_id = trigger_match_->id();
						const QPixmap *pixmap = nullptr;
						if (type_id >= 0 && (size_t)type_id < countof(TriggerMarkerIcons) &&
							TriggerMarkerIcons[type_id])
							pixmap = get_pixmap(TriggerMarkerIcons[type_id]);

						const int icon_w = pixmap ? pixmap->width() : 0;
						const int icon_h = pixmap ? pixmap->height() : 0;
						const int gap = pixmap ? 4 : 0;

						const int content_w = icon_w + gap + text_w;
						const int content_h = max(icon_h, text_h);

						/* 内容左上角:右侧留 pad 间距,垂直居中于 y_trig */
						const QPoint point(
							pp.right() - content_w - pad * 2,
							y_trig - content_h / 2);

						/* 黄色圆角背景(同 LogicSignal 触发标记) */
						p.setPen(QPen(TriggerMarkerBackgroundColor.darker()));
						p.setBrush(TriggerMarkerBackgroundColor);
						p.drawRoundedRect(QRectF(point.x(), point.y(),
							content_w, content_h).adjusted(-pad, -pad, pad, pad),
							pad, pad);

						/* 图标 */
						int x_cursor = point.x();
						if (pixmap)
						{
							const int icon_y = point.y() + (content_h - icon_h) / 2;
							p.drawPixmap(QPoint(x_cursor, icon_y), *pixmap);
							x_cursor += icon_w + gap;
						}

						/* 电压门限文本(深色,在黄底上可读) */
						p.setPen(QPen(QColor(0x33, 0x33, 0x33)));
						p.drawText(QRect(x_cursor, point.y(), text_w, content_h),
							Qt::AlignVCenter | Qt::AlignLeft, trig_text);
					}

					if (show_hover_marker_)
						paint_hover_marker(p);
				}

				if ((display_type_ == DisplayConverted) || (display_type_ == DisplayBoth))
					LogicSignal::paint_fore(p, pp);
			}

			void AnalogSignal::paint_grid(QPainter &p, int y, int left, int right)
			{
				bool was_antialiased = p.testRenderHint(QPainter::Antialiasing);
				p.setRenderHint(QPainter::Antialiasing, false);

				if (pos_vdivs_ > 0)
				{
					p.setPen(QPen(GridMajorColor, 1, Qt::DashLine));
					for (int i = 1; i <= pos_vdivs_; i++)
					{
						const float dy = i * div_height_;
						p.drawLine(QLineF(left, y - dy, right, y - dy));
					}
				}

				if ((pos_vdivs_ > 0) && show_analog_minor_grid_)
				{
					p.setPen(QPen(GridMinorColor, 1, Qt::DashLine));
					for (int i = 0; i < pos_vdivs_; i++)
					{
						const float dy = i * div_height_;
						const float dy25 = dy + (0.25 * div_height_);
						const float dy50 = dy + (0.50 * div_height_);
						const float dy75 = dy + (0.75 * div_height_);
						p.drawLine(QLineF(left, y - dy25, right, y - dy25));
						p.drawLine(QLineF(left, y - dy50, right, y - dy50));
						p.drawLine(QLineF(left, y - dy75, right, y - dy75));
					}
				}

				if (neg_vdivs_ > 0)
				{
					p.setPen(QPen(GridMajorColor, 1, Qt::DashLine));
					for (int i = 1; i <= neg_vdivs_; i++)
					{
						const float dy = i * div_height_;
						p.drawLine(QLineF(left, y + dy, right, y + dy));
					}
				}

				if ((pos_vdivs_ > 0) && show_analog_minor_grid_)
				{
					p.setPen(QPen(GridMinorColor, 1, Qt::DashLine));
					for (int i = 0; i < neg_vdivs_; i++)
					{
						const float dy = i * div_height_;
						const float dy25 = dy + (0.25 * div_height_);
						const float dy50 = dy + (0.50 * div_height_);
						const float dy75 = dy + (0.75 * div_height_);
						p.drawLine(QLineF(left, y + dy25, right, y + dy25));
						p.drawLine(QLineF(left, y + dy50, right, y + dy50));
						p.drawLine(QLineF(left, y + dy75, right, y + dy75));
					}
				}

				p.setRenderHint(QPainter::Antialiasing, was_antialiased);
			}

			void AnalogSignal::paint_trace(QPainter &p,
										   const shared_ptr<pv::data::AnalogSegment> &segment,
										   int y, int left, const int64_t start, const int64_t end,
										   const double pixels_offset, const double samples_per_pixel)
			{
				if (end <= start)
					return;

				bool paint_thr_dots =
					(base_->get_conversion_type() != data::SignalBase::NoConversion) &&
					(conversion_threshold_disp_mode_ == GlobalSettings::ConvThrDispMode_Dots);

				vector<double> thresholds;
				if (paint_thr_dots)
					thresholds = base_->get_conversion_thresholds();

				// Calculate and paint the sampling points if enabled and useful
				GlobalSettings settings;
				const bool show_sampling_points =
					(show_sampling_points_ || paint_thr_dots) && (samples_per_pixel < 0.25);

				p.setPen(base_->color());

				const int64_t points_count = end - start + 1;

				QPointF *points = new QPointF[points_count];
				QPointF *point = points;

				vector<QRectF> sampling_points[3];

				int64_t sample_count = min(points_count, TracePaintBlockSize);
				int64_t block_sample = 0;
				float *sample_block = new float[TracePaintBlockSize];
				segment->get_samples(start, start + sample_count, sample_block);

				if (show_hover_marker_)
					reset_pixel_values();

				const int w = 2;
				for (int64_t sample = start; sample <= end; sample++, block_sample++)
				{

					// Fetch next block of samples if we finished the current one
					if (block_sample == TracePaintBlockSize)
					{
						block_sample = 0;
						sample_count = min(points_count - sample, TracePaintBlockSize);
						segment->get_samples(sample, sample + sample_count, sample_block);
					}

					const float abs_x = sample / samples_per_pixel - pixels_offset;
					const float x = left + abs_x;

					*point++ = QPointF(x, y - sample_block[block_sample] * scale_);

					// Generate the pixel<->value lookup table for the mouse hover
					if (show_hover_marker_)
						process_next_sample_value(abs_x, sample_block[block_sample]);

					// Create the sampling points if needed
					if (show_sampling_points)
					{
						int idx = 0; // Neutral

						if (paint_thr_dots)
						{
							if (thresholds.size() == 1)
								idx = (sample_block[block_sample] >= thresholds[0]) ? 2 : 1;
							else if (thresholds.size() == 2)
							{
								if (sample_block[block_sample] > thresholds[1])
									idx = 2; // High
								else if (sample_block[block_sample] < thresholds[0])
									idx = 1; // Low
							}
						}

						sampling_points[idx].emplace_back(x - (w / 2), y - sample_block[block_sample] * scale_ - (w / 2), w, w);
					}
				}
				delete[] sample_block;

				// QPainter::drawPolyline() is slow, let's paint the lines ourselves
				if (sine_interpolation_ && (samples_per_pixel < 1.0))
				{
					// Fit a monotone cubic Hermite curve through the real samples.
					// Unlike cosine interpolation, this preserves linear segments exactly.
					std::vector<double> secants(points_count - 1);
					for (int64_t i = 0; i < points_count - 1; i++)
					{
						const double dx = points[i + 1].x() - points[i].x();
						secants[i] = (dx > 0.0)
							? (points[i + 1].y() - points[i].y()) / dx
							: 0.0;
					}

					std::vector<double> tangents(points_count);
					tangents.front() = secants.front();
					tangents.back() = secants.back();
					for (int64_t i = 1; i < points_count - 1; i++)
					{
						const double h0 = points[i].x() - points[i - 1].x();
						const double h1 = points[i + 1].x() - points[i].x();
						const double d0 = secants[i - 1];
						const double d1 = secants[i];

						if ((h0 <= 0.0) || (h1 <= 0.0) || (d0 * d1 <= 0.0))
						{
							tangents[i] = 0.0;
						}
						else
						{
							const double w1 = 2.0 * h1 + h0;
							const double w2 = h1 + 2.0 * h0;
							tangents[i] = (w1 + w2) / (w1 / d0 + w2 / d1);
						}
					}

					for (int64_t i = 1; i < points_count; i++)
					{
						const QPointF &p0 = points[i - 1];
						const QPointF &p1 = points[i];
						const double dx = p1.x() - p0.x();
						if (dx <= 1.0)
						{
							p.drawLine(p0, p1);
							continue;
						}

						const double subdivision = 16.0;
						const int steps = max(1, (int)ceil(dx * subdivision));
						const double h = dx;
						const double m0 = tangents[i - 1];
						const double m1 = tangents[i];
						QPointF prev = p0;
						for (int s = 1; s <= steps; s++)
						{
							const double t = (double)s / steps;
							const double t2 = t * t;
							const double t3 = t2 * t;
							const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
							const double h10 = t3 - 2.0 * t2 + t;
							const double h01 = -2.0 * t3 + 3.0 * t2;
							const double h11 = t3 - t2;
							const double y = h00 * p0.y() + h10 * h * m0 +
								h01 * p1.y() + h11 * h * m1;
							const QPointF cur(p0.x() + (p1.x() - p0.x()) * t, y);
							p.drawLine(prev, cur);
							prev = cur;
						}
					}
				}
				else
				{
					for (int64_t i = 1; i < points_count; i++)
						p.drawLine(points[i - 1], points[i]);
				}

				if (show_sampling_points)
				{
					if (paint_thr_dots)
					{
						p.setPen(SamplingPointColorNe);
						p.drawRects(sampling_points[0].data(), sampling_points[0].size());
						p.setPen(SamplingPointColorLo);
						p.drawRects(sampling_points[1].data(), sampling_points[1].size());
						p.setPen(SamplingPointColorHi);
						p.drawRects(sampling_points[2].data(), sampling_points[2].size());
					}
					else
					{
						p.setPen(SamplingPointColor);
						p.drawRects(sampling_points[0].data(), sampling_points[0].size());
					}
				}

				delete[] points;
			}

			void AnalogSignal::paint_envelope(QPainter &p,
											  const shared_ptr<pv::data::AnalogSegment> &segment,
											  int y, int left, const int64_t start, const int64_t end,
											  const double pixels_offset, const double samples_per_pixel)
			{
				using pv::data::AnalogSegment;

				// Note: Envelope painting currently doesn't generate a pixel<->value lookup table
				if (show_hover_marker_)
					reset_pixel_values();

				AnalogSegment::EnvelopeSection e;
				segment->get_envelope_section(e, start, end, samples_per_pixel);

				if (e.length < 2)
					return;

				p.setPen(QPen(Qt::NoPen));
				p.setBrush(base_->color());

				QRectF *const rects = new QRectF[e.length];
				QRectF *rect = rects;

				for (uint64_t sample = 0; sample < e.length - 1; sample++)
				{
					const float x = ((e.scale * sample + e.start) /
										 samples_per_pixel -
									 pixels_offset) +
									left;

					const AnalogSegment::EnvelopeSample *const s = e.samples + sample;

					// Fill pixel<->value lookup table for hover marker
					if (show_hover_marker_)
					{
						const float avg_value = (s->min + s->max) / 2;
						const int pixel_pos = (int)x;
						if (pixel_pos >= 0)
						{
							while ((int)value_at_pixel_pos_.size() <= pixel_pos)
								value_at_pixel_pos_.push_back(std::numeric_limits<float>::quiet_NaN());
							value_at_pixel_pos_[pixel_pos] = avg_value;
						}
					}

					// We overlap this sample with the next so that vertical
					// gaps do not appear during steep rising or falling edges
					const float b = y - max(s->max, (s + 1)->min) * scale_;
					const float t = y - min(s->min, (s + 1)->max) * scale_;

					float h = b - t;
					if (h >= 0.0f && h <= 1.0f)
						h = 1.0f;
					if (h <= 0.0f && h >= -1.0f)
						h = -1.0f;

					*rect++ = QRectF(x, t, 1.0f, h);
				}

				p.drawRects(rects, e.length);

				delete[] rects;
				delete[] e.samples;
			}

			shared_ptr<pv::data::AnalogSegment> AnalogSignal::get_analog_segment_to_paint() const
			{
				shared_ptr<pv::data::AnalogSegment> segment;

				const deque<shared_ptr<pv::data::AnalogSegment>> &segments =
					base_->analog_data()->analog_segments();

				if (!segments.empty())
				{
					if (segment_display_mode_ == ShowLastSegmentOnly)
						segment = segments.back();

					if ((segment_display_mode_ == ShowSingleSegmentOnly) ||
						(segment_display_mode_ == ShowLastCompleteSegmentOnly))
					{
						try
						{
							segment = segments.at(current_segment_);
						}
						catch (out_of_range &)
						{
							qDebug() << "Current analog segment out of range for signal" << base_->name() << ":" << current_segment_;
						}
					}
				}

				return segment;
			}

			float AnalogSignal::get_resolution(int scale_index)
			{
				const float seq[] = {1.0f, 2.0f, 5.0f};

				const int offset = numeric_limits<int>::max() / (2 * countof(seq));
				const div_t d = div((int)(scale_index + countof(seq) * offset),
									countof(seq));

				return powf(10.0f, d.quot - offset) * seq[d.rem];
			}

			void AnalogSignal::update_scale()
			{
				resolution_ = get_resolution(scale_index_);
				scale_ = div_height_ / resolution_;
			}

			void AnalogSignal::update_logic_level_offsets()
			{
				const int signal_margin = QFontMetrics(QApplication::font()).height() / 2;

				const int ph = min(pos_vdivs_, 1) * div_height_;
				const int nh = min(neg_vdivs_, 1) * div_height_;

				high_level_offset_ = -ph + signal_margin + 0.5f;
				low_level_offset_ = nh - signal_margin - 0.5f;
			}

			void AnalogSignal::update_conversion_widgets()
			{
				SignalBase::ConversionType conv_type = base_->get_conversion_type();

				// Enable or disable widgets depending on conversion state
				conv_threshold_cb_->setEnabled(conv_type != SignalBase::NoConversion);
				display_type_cb_->setEnabled(conv_type != SignalBase::NoConversion);

				conv_threshold_cb_->clear();

				vector<pair<QString, int>> presets = base_->get_conversion_presets();

				// Prevent the combo box from firing the "edit text changed" signal
				// as that would involuntarily select the first entry
				conv_threshold_cb_->blockSignals(true);

				// Set available options depending on chosen conversion
				for (pair<QString, int> &preset : presets)
					conv_threshold_cb_->addItem(preset.first, preset.second);

				map<QString, QVariant> options = base_->get_conversion_options();

				if (conv_type == SignalBase::A2LConversionByThreshold)
				{
					const vector<double> thresholds = base_->get_conversion_thresholds(
						SignalBase::A2LConversionByThreshold, true);
					conv_threshold_cb_->addItem(
						QString("%1V").arg(QString::number(thresholds[0], 'f', 1)), -1);
				}

				if (conv_type == SignalBase::A2LConversionBySchmittTrigger)
				{
					const vector<double> thresholds = base_->get_conversion_thresholds(
						SignalBase::A2LConversionBySchmittTrigger, true);
					conv_threshold_cb_->addItem(QString("%1V/%2V").arg(
													QString::number(thresholds[0], 'f', 1),
													QString::number(thresholds[1], 'f', 1)),
												-1);
				}

				int preset_id = base_->get_current_conversion_preset();
				conv_threshold_cb_->setCurrentIndex(
					conv_threshold_cb_->findData(preset_id));

				conv_threshold_cb_->blockSignals(false);
			}

			vector<data::LogicSegment::EdgePair> AnalogSignal::get_nearest_level_changes(uint64_t sample_pos)
			{
				assert(base_);
				assert(owner_);

				// Return if there's no logic data or we're showing only the analog trace
				if (!base_->logic_data() || (display_type_ == DisplayAnalog))
					return vector<data::LogicSegment::EdgePair>();

				if (sample_pos == 0)
					return vector<LogicSegment::EdgePair>();

				shared_ptr<LogicSegment> segment = get_logic_segment_to_paint();
				if (!segment || (segment->get_sample_count() == 0))
					return vector<LogicSegment::EdgePair>();

				const View *view = owner_->view();
				assert(view);
				const double samples_per_pixel = base_->get_samplerate() * view->scale();

				vector<LogicSegment::EdgePair> edges;

				segment->get_surrounding_edges(edges, sample_pos,
											   samples_per_pixel / LogicSignal::Oversampling, 0);

				if (edges.empty())
					return vector<LogicSegment::EdgePair>();

				return edges;
			}

			void AnalogSignal::perform_autoranging(bool keep_divs, bool force_update)
			{
				const deque<shared_ptr<pv::data::AnalogSegment>> &segments =
					base_->analog_data()->analog_segments();

				if (segments.empty())
					return;

				double min = 0, max = 0;

				for (const shared_ptr<pv::data::AnalogSegment> &segment : segments)
				{
					pair<double, double> mm = segment->get_min_max();
					min = std::min(min, mm.first);
					max = std::max(max, mm.second);
				}

				if ((min == signal_min_) && (max == signal_max_) && !force_update)
					return;

				signal_min_ = min;
				signal_max_ = max;

				const int old_pos_vdivs = pos_vdivs_;
				const int old_neg_vdivs = neg_vdivs_;

				// If we're allowed to alter the div assignment...
				if (!keep_divs)
				{
					// Use all divs for the positive range if there are no negative values
					if ((min == 0) && (neg_vdivs_ > 0))
					{
						pos_vdivs_ += neg_vdivs_;
						neg_vdivs_ = 0;
					}

					// Split up the divs if there are negative values but no negative divs
					if ((min < 0) && (neg_vdivs_ == 0))
					{
						neg_vdivs_ = pos_vdivs_ / 2;
						pos_vdivs_ -= neg_vdivs_;
					}
				}

				const bool showing_logic = (display_type_ == DisplayConverted) || (display_type_ == DisplayBoth);

				// If there is still no positive div when we need it, add one
				// (this can happen when pos_vdivs==neg_vdivs==0)
				if (((max > 0) && (pos_vdivs_ == 0)) || showing_logic)
				{
					pos_vdivs_ = 1;
				}

				// If there is still no negative div when we need it, add one
				// (this can happen when pos_vdivs was 0 or 1 when trying to split)
				if (((min < 0) && (neg_vdivs_ == 0)) || showing_logic)
				{
					neg_vdivs_ = 1;
				}

				if ((pos_vdivs_ != old_pos_vdivs) || (neg_vdivs_ != old_neg_vdivs))
					owner_->extents_changed(false, true);

				double min_value_per_div;
				if ((pos_vdivs_ > 0) && (neg_vdivs_ > 0))
					min_value_per_div = std::max(max / pos_vdivs_, -min / neg_vdivs_);
				else if (pos_vdivs_ > 0)
					min_value_per_div = max / pos_vdivs_;
				else
					min_value_per_div = -min / neg_vdivs_;

				// Find first scale value that is bigger than the value we need
				for (int i = MinScaleIndex; i < MaxScaleIndex; i++)
					if (get_resolution(i) > min_value_per_div)
					{
						scale_index_ = i;
						break;
					}

				update_scale();
			}

			void AnalogSignal::reset_pixel_values()
			{
				value_at_pixel_pos_.clear();
				current_pixel_pos_ = -1;
				prev_value_at_pixel_ = std::numeric_limits<float>::quiet_NaN();
			}

			void AnalogSignal::process_next_sample_value(float x, float value)
			{
				// Note: NAN is used to indicate the non-existance of a value at this pixel

				if (std::isnan(prev_value_at_pixel_))
				{
					if (x < 0)
					{
						min_value_at_pixel_ = value;
						max_value_at_pixel_ = value;
						prev_value_at_pixel_ = value;
						current_pixel_pos_ = x;
					}
					else
						prev_value_at_pixel_ = std::numeric_limits<float>::quiet_NaN();
				}

				const int pixel_pos = (int)(x + 0.5);

				if (pixel_pos > current_pixel_pos_)
				{
					if (pixel_pos - current_pixel_pos_ == 1)
					{
						if (std::isnan(prev_value_at_pixel_))
						{
							value_at_pixel_pos_.push_back(prev_value_at_pixel_);
						}
						else
						{
							// Average the min/max range to create one value for the previous pixel
							const float avg = (min_value_at_pixel_ + max_value_at_pixel_) / 2;
							value_at_pixel_pos_.push_back(avg);
						}
					}
					else
					{
						// Interpolate values to create values for the intermediate pixels
						const float start_value = prev_value_at_pixel_;
						const float end_value = value;
						const int steps = fabs(pixel_pos - current_pixel_pos_);
						const double gradient = (end_value - start_value) / steps;
						for (int i = 0; i < steps; i++)
						{
							if (current_pixel_pos_ + i < 0)
								continue;
							value_at_pixel_pos_.push_back(start_value + i * gradient);
						}
					}

					min_value_at_pixel_ = value;
					max_value_at_pixel_ = value;
					prev_value_at_pixel_ = value;
					current_pixel_pos_ = pixel_pos;
				}
				else
				{
					// Another sample for the same pixel
					if (value < min_value_at_pixel_)
						min_value_at_pixel_ = value;
					if (value > max_value_at_pixel_)
						max_value_at_pixel_ = value;
				}
			}

			void AnalogSignal::populate_popup_form(QWidget *parent, QFormLayout *form)
			{
				// Add the standard options
				Signal::populate_popup_form(parent, form);

				/* ADC电平触发设置：触发方向 + 电压门限。置于颜色选项之下、
				 * div 设置之上。门限 spinbox 样式参照 channels.cpp 的
				 * voltage_level_spinbox_。仅在停止态可修改。 */
				trigger_type_cb_ = new QComboBox(parent);
				trigger_type_cb_->addItem(tr("None"), 0);
				trigger_type_cb_->addItem(tr("Rising"), TriggerMatchType::RISING->id());
				trigger_type_cb_->addItem(tr("Falling"), TriggerMatchType::FALLING->id());
				trigger_type_cb_->addItem(tr("Edge"), TriggerMatchType::EDGE->id());
				trigger_type_cb_->addItem(tr("Over"), TriggerMatchType::OVER->id());
				trigger_type_cb_->addItem(tr("Under"), TriggerMatchType::UNDER->id());
				{
					const int cur_id = trigger_match_ ? trigger_match_->id() : 0;
					const int idx = trigger_type_cb_->findData(cur_id);
					if (idx >= 0)
						trigger_type_cb_->setCurrentIndex(idx);
				}
				connect(trigger_type_cb_, SIGNAL(currentIndexChanged(int)),
						this, SLOT(on_analog_trigger_changed()));

				trigger_level_sb_ = new QDoubleSpinBox(parent);
				trigger_level_sb_->setRange(0.0, 5.0);
				trigger_level_sb_->setSingleStep(0.01);
				trigger_level_sb_->setDecimals(2);
				trigger_level_sb_->setSuffix(tr(" V"));
				trigger_level_sb_->setStyleSheet(
					"QDoubleSpinBox { padding-right: 24px; }"
					"QDoubleSpinBox::up-button { subcontrol-origin: margin; subcontrol-position: right top; width: 18px; height: 14px; margin-top: 1px; }"
					"QDoubleSpinBox::down-button { subcontrol-origin: margin; subcontrol-position: right bottom; width: 18px; height: 14px; margin-bottom: 1px; }");
				if (!std::isnan(analog_trigger_value_))
					trigger_level_sb_->setValue(analog_trigger_value_);
				else
					trigger_level_sb_->setValue(1.65);
				trigger_level_sb_->setEnabled(trigger_match_ != nullptr);
				connect(trigger_level_sb_, SIGNAL(valueChanged(double)),
						this, SLOT(on_analog_trigger_changed()));

				QHBoxLayout *const atrig_layout = new QHBoxLayout;
				atrig_layout->addWidget(trigger_type_cb_);
				atrig_layout->addWidget(trigger_level_sb_);
				form->addRow(tr("ADC trigger"), atrig_layout);

				// 仅在停止态允许修改触发
				if (session_.get_capture_state() != Session::Stopped)
				{
					trigger_type_cb_->setEnabled(false);
					trigger_level_sb_->setEnabled(false);
				}

				// Add div-related settings
				pvdiv_sb_ = new QSpinBox(parent);
				pvdiv_sb_->setRange(0, MaximumVDivs);
				pvdiv_sb_->setValue(pos_vdivs_);
				pvdiv_sb_->setEnabled(!autoranging_);
				connect(pvdiv_sb_, SIGNAL(valueChanged(int)),
						this, SLOT(on_pos_vdivs_changed(int)));
				form->addRow(tr("Number of pos vertical divs"), pvdiv_sb_);

				nvdiv_sb_ = new QSpinBox(parent);
				nvdiv_sb_->setRange(0, MaximumVDivs);
				nvdiv_sb_->setValue(neg_vdivs_);
				nvdiv_sb_->setEnabled(!autoranging_);
				connect(nvdiv_sb_, SIGNAL(valueChanged(int)),
						this, SLOT(on_neg_vdivs_changed(int)));
				form->addRow(tr("Number of neg vertical divs"), nvdiv_sb_);

				div_height_sb_ = new QSpinBox(parent);
				div_height_sb_->setRange(20, 1000);
				div_height_sb_->setSingleStep(5);
				div_height_sb_->setSuffix(tr(" pixels"));
				div_height_sb_->setValue(div_height_);
				connect(div_height_sb_, SIGNAL(valueChanged(int)),
						this, SLOT(on_div_height_changed(int)));
				form->addRow(tr("Div height"), div_height_sb_);

				// Add the vertical resolution
				resolution_cb_ = new QComboBox(parent);
				resolution_cb_->setEnabled(!autoranging_);

				for (int i = MinScaleIndex; i < MaxScaleIndex; i++)
				{
					const QString label = QString("%1").arg(get_resolution(i));
					resolution_cb_->insertItem(0, label, QVariant(i));
				}

				int cur_idx = resolution_cb_->findData(QVariant(scale_index_));
				resolution_cb_->setCurrentIndex(cur_idx);

				connect(resolution_cb_, SIGNAL(currentIndexChanged(int)),
						this, SLOT(on_resolution_changed(int)));

				QGridLayout *const vdiv_layout = new QGridLayout;
				QLabel *const vdiv_unit = new QLabel(tr("V/div"));
				vdiv_layout->addWidget(resolution_cb_, 0, 0);
				vdiv_layout->addWidget(vdiv_unit, 0, 1);

				form->addRow(tr("Vertical resolution"), vdiv_layout);

				// Add the autoranging checkbox
				QCheckBox *autoranging_cb = new QCheckBox();
				autoranging_cb->setCheckState(autoranging_ ? Qt::Checked : Qt::Unchecked);

				connect(autoranging_cb, SIGNAL(stateChanged(int)),
						this, SLOT(on_autoranging_changed(int)));

				form->addRow(tr("Autoranging"), autoranging_cb);

				// Add the sine interpolation checkbox for smooth ADC display
				QCheckBox *sine_interp_cb = new QCheckBox(parent);
				sine_interp_cb->setCheckState(sine_interpolation_ ? Qt::Checked : Qt::Unchecked);

				connect(sine_interp_cb, SIGNAL(stateChanged(int)),
						this, SLOT(on_sine_interpolation_changed(int)));

				form->addRow(tr("Sine interpolation"), sine_interp_cb);

				// Add the conversion type dropdown
				conversion_cb_ = new QComboBox();

				conversion_cb_->addItem(tr("none"),
										SignalBase::NoConversion);
				conversion_cb_->addItem(tr("to logic via threshold"),
										SignalBase::A2LConversionByThreshold);
				conversion_cb_->addItem(tr("to logic via schmitt-trigger"),
										SignalBase::A2LConversionBySchmittTrigger);

				cur_idx = conversion_cb_->findData(QVariant(base_->get_conversion_type()));
				conversion_cb_->setCurrentIndex(cur_idx);

				form->addRow(tr("Conversion"), conversion_cb_);

				connect(conversion_cb_, SIGNAL(currentIndexChanged(int)),
						this, SLOT(on_conversion_changed(int)));

				// Add the conversion threshold settings
				conv_threshold_cb_ = new QComboBox();
				conv_threshold_cb_->setEditable(true);

				form->addRow(tr("Conversion threshold(s)"), conv_threshold_cb_);

				connect(conv_threshold_cb_, SIGNAL(currentIndexChanged(int)),
						this, SLOT(on_conv_threshold_changed(int)));
				connect(conv_threshold_cb_, SIGNAL(editTextChanged(const QString &)),
						this, SLOT(on_conv_threshold_changed())); // index will be -1

				// Add the display type dropdown
				display_type_cb_ = new QComboBox();

				display_type_cb_->addItem(tr("analog"), DisplayAnalog);
				display_type_cb_->addItem(tr("converted"), DisplayConverted);
				display_type_cb_->addItem(tr("analog+converted"), DisplayBoth);

				cur_idx = display_type_cb_->findData(QVariant(display_type_));
				display_type_cb_->setCurrentIndex(cur_idx);

				form->addRow(tr("Show traces for"), display_type_cb_);

				connect(display_type_cb_, SIGNAL(currentIndexChanged(int)),
						this, SLOT(on_display_type_changed(int)));

				// Update the conversion widget contents and states
				update_conversion_widgets();
			}

			void AnalogSignal::modify_trigger()
			{
				auto trigger = session_.session()->trigger();
				auto new_trigger = session_.device_manager().context()->create_trigger("pulseview");

				if (trigger) {
					for (auto stage : trigger->stages()) {
						const auto &matches = stage->matches();
						// 跳过仅含本通道match的stage(移除后会变空)
						if (none_of(matches.begin(), matches.end(),
							[&](shared_ptr<TriggerMatch> match) {
								return match->channel() != base_->channel(); }))
							continue;

						auto new_stage = new_trigger->add_stage();
						for (auto match : stage->matches()) {
							if (match->channel() == base_->channel())
								continue;
							// 保留其它通道触发的门限值(逻辑通道为NAN,与无value版本等价)
							new_stage->add_match(match->channel(), match->type(), match->value());
						}
					}
				}

				if (trigger_match_) {
					// 多数设备仅支持单stage,统一放入末尾stage
					if (new_trigger->stages().empty())
						new_trigger->add_stage();

					new_trigger->stages().back()->add_match(base_->channel(),
						trigger_match_, analog_trigger_value_);
				}

				session_.session()->set_trigger(
					new_trigger->stages().empty() ? nullptr : new_trigger);

				if (owner_)
					owner_->row_item_appearance_changed(false, true);
			}

			void AnalogSignal::on_analog_trigger_changed()
			{
				if (!trigger_type_cb_ || !trigger_level_sb_)
					return;

				const int type_id = trigger_type_cb_->currentData().toInt();
				if (type_id == 0)
					trigger_match_ = nullptr;
				else
					trigger_match_ = TriggerMatchType::get(type_id);

				analog_trigger_value_ = (float)trigger_level_sb_->value();
				trigger_level_sb_->setEnabled(trigger_match_ != nullptr);

				modify_trigger();
			}

			/* 空实现:模拟通道的触发标记由 paint_fore 自行绘制(带电压门限),
			 * 抑制 LogicSignal::paint_fore 中的逻辑图标式标记,避免 DisplayBoth 重复。 */
			void AnalogSignal::paint_trigger_marker(QPainter &p, ViewItemPaintParams &pp)
			{
				(void)p;
				(void)pp;
			}

			void AnalogSignal::paint_hover_marker(QPainter &p)
			{
				Trace::paint_hover_marker(p);

				// 如果没有有效的电压值，不绘制标签
				if (std::isnan(value_at_hover_pos_))
					return;

				const View *view = owner_->view();
				if (!view)
				{
					return;
				}

				const int x = view->hover_point().x();
				if (x == -1)
					return;

				const pair<int, int> extents = v_extents();
				const int visual_y = get_visual_y();
				const int top = visual_y + extents.first;
				const int bottom = visual_y + extents.second;

				int voltage_y = visual_y - (int)(value_at_hover_pos_ * scale_);
				voltage_y = std::min(std::max(voltage_y, top), bottom);

				// 绘制交汇点标记
				p.setRenderHint(QPainter::Antialiasing, true);
				p.setBrush(QColor(255, 200, 0));
				p.setPen(QPen(Qt::black, 1));
				p.drawEllipse(QPointF(x, voltage_y), 3, 3);

				QString voltage_text;
				double abs_v = fabs(value_at_hover_pos_);
				if (abs_v >= 1.0)
					voltage_text = QString::asprintf("%.3f V", value_at_hover_pos_);
				else if (abs_v >= 0.001)
					voltage_text = QString::asprintf("%.2f mV", value_at_hover_pos_ * 1000);
				else
					voltage_text = QString::asprintf("%.2f uV", value_at_hover_pos_ * 1000000);

				QFontMetrics fm(p.font());
				QRect text_rect = fm.boundingRect(voltage_text);
				int label_width = text_rect.width() + 16;
				int label_height = text_rect.height() + 8;

				int label_x = x + 12;
				int label_y = voltage_y - label_height / 2;

				int viewport_width = view->viewport()->width();
				int viewport_height = view->viewport()->height();
				if (label_x + label_width > viewport_width)
					label_x = x - label_width - 12;
				if (label_y < 0)
					label_y = 0;
				if (label_y + label_height > viewport_height)
					label_y = viewport_height - label_height;

				p.setPen(Qt::NoPen);
				p.setBrush(QColor(40, 40, 40, 220));
				p.drawRoundedRect(label_x, label_y, label_width, label_height, 4, 4);

				// 绘制电压文本
				p.setPen(QColor(255, 255, 255));
				p.drawText(label_x + 8, label_y + label_height - fm.descent() - 4, voltage_text);

				p.setRenderHint(QPainter::Antialiasing, false);
			}

			void AnalogSignal::hover_point_changed(const QPoint &hp)
			{
				Signal::hover_point_changed(hp);

				// Note: Even though the view area begins at 0, we exclude 0 because
				// that's also the value given when the cursor is over the header to the
				// left of the trace paint area
				if (hp.x() <= 0)
				{
					value_at_hover_pos_ = std::numeric_limits<float>::quiet_NaN();
				}
				else
				{
					if ((size_t)hp.x() < value_at_pixel_pos_.size())
						value_at_hover_pos_ = value_at_pixel_pos_.at(hp.x());
					else
						value_at_hover_pos_ = std::numeric_limits<float>::quiet_NaN();
				}
			}

			void AnalogSignal::on_setting_changed(const QString &key, const QVariant &value)
			{
				Signal::on_setting_changed(key, value);

				if (key == GlobalSettings::Key_View_ShowSamplingPoints)
					show_sampling_points_ = value.toBool();

				if (key == GlobalSettings::Key_View_FillSignalHighAreas)
					fill_high_areas_ = value.toBool();

				if (key == GlobalSettings::Key_View_FillSignalHighAreaColor)
					high_fill_color_ = QColor::fromRgba(value.value<uint32_t>());

				if (key == GlobalSettings::Key_View_ShowAnalogMinorGrid)
					show_analog_minor_grid_ = value.toBool();

				if (key == GlobalSettings::Key_View_ConversionThresholdDispMode)
				{
					conversion_threshold_disp_mode_ = value.toInt();

					if (owner_)
						owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_min_max_changed(float min, float max)
			{
				if (autoranging_)
					perform_autoranging(false, false);
				else
				{
					if (min < signal_min_)
						signal_min_ = min;
					if (max > signal_max_)
						signal_max_ = max;
				}
			}

			void AnalogSignal::on_pos_vdivs_changed(int vdivs)
			{
				if (vdivs == pos_vdivs_)
					return;

				pos_vdivs_ = vdivs;

				// There has to be at least one div, positive or negative
				if ((neg_vdivs_ == 0) && (pos_vdivs_ == 0))
				{
					pos_vdivs_ = 1;
					if (pvdiv_sb_)
						pvdiv_sb_->setValue(pos_vdivs_);
				}

				if (autoranging_)
				{
					perform_autoranging(true, true);

					// It could be that a positive or negative div was added, so update
					if (pvdiv_sb_)
					{
						pvdiv_sb_->setValue(pos_vdivs_);
						nvdiv_sb_->setValue(neg_vdivs_);
					}
				}

				update_logic_level_offsets();

				if (owner_)
				{
					// Call order is important, otherwise the lazy event handler won't work
					owner_->extents_changed(false, true);
					owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_neg_vdivs_changed(int vdivs)
			{
				if (vdivs == neg_vdivs_)
					return;

				neg_vdivs_ = vdivs;

				// There has to be at least one div, positive or negative
				if ((neg_vdivs_ == 0) && (pos_vdivs_ == 0))
				{
					pos_vdivs_ = 1;
					if (pvdiv_sb_)
						pvdiv_sb_->setValue(pos_vdivs_);
				}

				if (autoranging_)
				{
					perform_autoranging(true, true);

					// It could be that a positive or negative div was added, so update
					if (pvdiv_sb_)
					{
						pvdiv_sb_->setValue(pos_vdivs_);
						nvdiv_sb_->setValue(neg_vdivs_);
					}
				}

				update_logic_level_offsets();

				if (owner_)
				{
					// Call order is important, otherwise the lazy event handler won't work
					owner_->extents_changed(false, true);
					owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_div_height_changed(int height)
			{
				div_height_ = height;
				update_logic_level_offsets();
				update_scale();

				if (owner_)
				{
					// Call order is important, otherwise the lazy event handler won't work
					owner_->extents_changed(false, true);
					owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_resolution_changed(int index)
			{
				scale_index_ = resolution_cb_->itemData(index).toInt();
				update_scale();

				if (owner_)
					owner_->row_item_appearance_changed(false, true);
			}

			void AnalogSignal::on_autoranging_changed(int state)
			{
				autoranging_ = (state == Qt::Checked);

				if (pvdiv_sb_)
					pvdiv_sb_->setEnabled(!autoranging_);
				if (nvdiv_sb_)
					nvdiv_sb_->setEnabled(!autoranging_);
				if (resolution_cb_)
					resolution_cb_->setEnabled(!autoranging_);

				if (autoranging_)
					perform_autoranging(false, true);

				if (owner_)
				{
					// Call order is important, otherwise the lazy event handler won't work
					owner_->extents_changed(false, true);
					owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_sine_interpolation_changed(int state)
			{
				sine_interpolation_ = (state == Qt::Checked);

				if (owner_)
					owner_->row_item_appearance_changed(false, true);
			}

			void AnalogSignal::on_conversion_changed(int index)
			{
				SignalBase::ConversionType old_conv_type = base_->get_conversion_type();

				SignalBase::ConversionType conv_type =
					(SignalBase::ConversionType)(conversion_cb_->itemData(index).toInt());

				if (conv_type != old_conv_type)
				{
					base_->set_conversion_type(conv_type);
					update_conversion_widgets();

					if (conv_type == SignalBase::ConversionType::NoConversion)
						on_display_type_changed(DisplayType::DisplayAnalog);
					else
						on_display_type_changed(DisplayType::DisplayBoth);

					if (owner_)
						owner_->row_item_appearance_changed(false, true);
				}
			}

			void AnalogSignal::on_conv_threshold_changed(int index)
			{
				SignalBase::ConversionType conv_type = base_->get_conversion_type();

				// Note: index is set to -1 if the text in the combo box matches none of
				// the entries in the combo box

				if ((index == -1) && (conv_threshold_cb_->currentText().length() == 0))
					return;

				// The combo box entry with the custom value has user_data set to -1
				const int user_data = conv_threshold_cb_->findText(
					conv_threshold_cb_->currentText());

				const bool use_custom_thr = (index == -1) || (user_data == -1);

				if (conv_type == SignalBase::A2LConversionByThreshold && use_custom_thr)
				{
					// Not one of the preset values, try to parse the combo box text
					// Note: Regex loosely based on
					// https://txt2re.com/index-c++.php3?s=0.1V&1&-13
					QString re1 = "([+-]?\\d*[\\.,]?\\d*)"; // Float value
					QString re2 = "([a-zA-Z]*)";			// SI unit
					QRegExp regex(re1 + re2);

					const QString text = conv_threshold_cb_->currentText();
					if (!regex.exactMatch(text))
						return; // String doesn't match the regex

					QStringList tokens = regex.capturedTexts();

					// For now, we simply assume that the unit is volt without modifiers
					const double thr = tokens.at(1).toDouble();

					// Only restart the conversion if the threshold was updated.
					// We're starting a delayed conversion because the user may still be
					// typing and the UI would lag if we kept on restarting it immediately
					if (base_->set_conversion_option("threshold_value", thr))
						base_->start_conversion(true);
				}

				if (conv_type == SignalBase::A2LConversionBySchmittTrigger && use_custom_thr)
				{
					// Not one of the preset values, try to parse the combo box text
					// Note: Regex loosely based on
					// https://txt2re.com/index-c++.php3?s=0.1V/0.2V&2&14&-22&3&15
					QString re1 = "([+-]?\\d*[\\.,]?\\d*)"; // Float value
					QString re2 = "([a-zA-Z]*)";			// SI unit
					QString re3 = "\\/";					// Forward slash, not captured
					QString re4 = "([+-]?\\d*[\\.,]?\\d*)"; // Float value
					QString re5 = "([a-zA-Z]*)";			// SI unit
					QRegExp regex(re1 + re2 + re3 + re4 + re5);

					const QString text = conv_threshold_cb_->currentText();
					if (!regex.exactMatch(text))
						return; // String doesn't match the regex

					QStringList tokens = regex.capturedTexts();

					// For now, we simply assume that the unit is volt without modifiers
					const double low_thr = tokens.at(1).toDouble();
					const double high_thr = tokens.at(3).toDouble();

					// Only restart the conversion if one of the options was updated.
					// We're starting a delayed conversion because the user may still be
					// typing and the UI would lag if we kept on restarting it immediately
					bool o1 = base_->set_conversion_option("threshold_value_low", low_thr);
					bool o2 = base_->set_conversion_option("threshold_value_high", high_thr);
					if (o1 || o2)
						base_->start_conversion(true); // Start delayed conversion
				}

				base_->set_conversion_preset((SignalBase::ConversionPreset)index);

				// Immediately start the conversion if we're not using custom values
				// (i.e. we're using one of the presets)
				if (!use_custom_thr)
					base_->start_conversion();
			}

			void AnalogSignal::on_delayed_conversion_starter()
			{
				base_->start_conversion();
			}

			void AnalogSignal::on_display_type_changed(int index)
			{
				const bool prev_showing_logic = (display_type_ == DisplayConverted) || (display_type_ == DisplayBoth);

				display_type_ = (DisplayType)(display_type_cb_->itemData(index).toInt());

				const bool showing_logic = (display_type_ == DisplayConverted) || (display_type_ == DisplayBoth);

				// If we show a logic trace, make sure we have at least one div for each
				// polarity as that's where we paint it
				if (showing_logic && !prev_showing_logic)
				{
					if (pos_vdivs_ == 0)
						on_pos_vdivs_changed(1);
					if (neg_vdivs_ == 0)
						on_neg_vdivs_changed(1);
				}

				if (owner_)
					owner_->row_item_appearance_changed(false, true);
			}

		} // namespace trace
	} // namespace views
} // namespace pv
