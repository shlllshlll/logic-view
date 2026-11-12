/*
 * This file is part of the PulseView project.
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

#include <libsigrokdecode/libsigrokdecode.h>

#include <cassert>
#include <vector>

#include <QRegularExpression>

#include <pv/data/decode/annotation.hpp>
#include <pv/data/decode/decoder.hpp>
#include "pv/data/decode/rowdata.hpp"

using std::vector;

namespace pv {
namespace data {
namespace decode {

Annotation::Annotation(uint64_t start_sample, uint64_t end_sample,
	const vector<QString>* texts, uint32_t ann_class_id, const RowData *data) :
	start_sample_(start_sample),
	end_sample_(end_sample),
	texts_(texts),
	ann_class_id_(ann_class_id),
	data_(data)
{
}

Annotation::Annotation(Annotation&& a) :
	start_sample_(a.start_sample_),
	end_sample_(a.end_sample_),
	texts_(a.texts_),
	ann_class_id_(a.ann_class_id_),
	data_(a.data_)
{
}

Annotation& Annotation::operator=(Annotation&& a)
{
	if (&a != this) {
		start_sample_ = a.start_sample_;
		end_sample_ = a.end_sample_;
		texts_ = a.texts_;
		ann_class_id_ = a.ann_class_id_;
		data_ = a.data_;
	}

	return *this;
}

const RowData* Annotation::row_data() const
{
	return data_;
}

const Row* Annotation::row() const
{
	return data_->row();
}

uint64_t Annotation::start_sample() const
{
	return start_sample_;
}

uint64_t Annotation::end_sample() const
{
	return end_sample_;
}

uint64_t Annotation::length() const
{
	return end_sample_ - start_sample_;
}

uint32_t Annotation::ann_class_id() const
{
	return ann_class_id_;
}

const QString Annotation::ann_class_name() const
{
	const AnnotationClass* ann_class =
		data_->row()->decoder()->get_ann_class_by_id(ann_class_id_);

	return QString(ann_class->name);
}

const QString Annotation::ann_class_description() const
{
	const AnnotationClass* ann_class =
		data_->row()->decoder()->get_ann_class_by_id(ann_class_id_);

	return QString(ann_class->description);
}

const vector<QString>* Annotation::annotations() const
{
	return texts_;
}

const QString Annotation::longest_annotation() const
{
	return texts_->front();
}

bool Annotation::visible() const
{
	const Row* row = data_->row();

	return (row->visible() && row->class_is_visible(ann_class_id_)
		&& row->decoder()->visible());
}

namespace {

QColor color_from_annotation_text(const vector<QString>* texts)
{
	static const QRegularExpression re(
		"(RGB|GRB|BRG|RBG|BGR|GBR)#([0-9a-fA-F]{6})(?!\\w)");

	if (!texts)
		return QColor();

	for (const QString &text : *texts) {
		const QRegularExpressionMatch match = re.match(text);
		if (!match.hasMatch())
			continue;

		bool ok = false;
		const uint32_t value = match.captured(2).toUInt(&ok, 16);
		if (!ok)
			continue;

		const QString order = match.captured(1);
		uint32_t rgb = value;
		if (order == "GRB")
			// G,R,B -> R,G,B
			rgb = ((value & 0x00ff00) << 8) |
			      ((value & 0xff0000) >> 8) |
			      (value & 0x0000ff);
		else if (order == "BRG")
			// B,R,G -> R,G,B
			rgb = ((value & 0x00ff00) << 8) |
			      ((value & 0x0000ff) << 8) |
			      ((value & 0xff0000) >> 16);
		else if (order == "RBG")
			// R,B,G -> R,G,B
			rgb = (value & 0xff0000) |
			      ((value & 0x0000ff) << 8) |
			      ((value & 0x00ff00) >> 8);
		else if (order == "BGR")
			// B,G,R -> R,G,B
			rgb = ((value & 0x0000ff) << 16) |
			      (value & 0x00ff00) |
			      ((value & 0xff0000) >> 16);
		else if (order == "GBR")
			// G,B,R -> R,G,B
			rgb = ((value & 0x0000ff) << 16) |
			      ((value & 0xff0000) >> 8) |
			      ((value & 0x00ff00) >> 8);

		return QColor::fromRgb(rgb);
	}

	return QColor();
}

} // namespace

const QColor Annotation::explicit_color() const
{
	return color_from_annotation_text(texts_);
}

const QColor Annotation::color() const
{
	const QColor ann_color = explicit_color();
	if (ann_color.isValid())
		return ann_color;

	return data_->row()->get_class_color(ann_class_id_);
}

const QColor Annotation::bright_color() const
{
	const QColor ann_color = explicit_color();
	if (ann_color.isValid())
		return ann_color.lighter();

	return data_->row()->get_bright_class_color(ann_class_id_);
}

const QColor Annotation::dark_color() const
{
	const QColor ann_color = explicit_color();
	if (ann_color.isValid())
		return ann_color.darker();

	return data_->row()->get_dark_class_color(ann_class_id_);
}

bool Annotation::operator<(const Annotation &other) const
{
	return (start_sample_ < other.start_sample_);
}

} // namespace decode
} // namespace data
} // namespace pv
