/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "struct_def.h"

#include "fields/all_fields.h"
#include "util.h"

StructDef::StructDef(std::string name, FieldList fields) : StructDef(name, fields, nullptr) {}
StructDef::StructDef(std::string name, FieldList fields, StructDef* parent)
    : ParentDef(name, fields, parent), total_size_(GetSize(true)) {}

PacketField* StructDef::GetNewField(const std::string& name, ParseLocation loc) const {
  if (fields_.HasBody()) {
    return new VariableLengthStructField(name, name_, loc);
  } else {
    return new StructField(name, name_, total_size_, loc);
  }
}

TypeDef::Type StructDef::GetDefinitionType() const {
  return TypeDef::Type::STRUCT;
}

void StructDef::GenSpecialize(std::ostream& s) const {
  if (parent_ == nullptr) {
    return;
  }
  s << "static " << name_ << "* Specialize(" << parent_->name_ << "* parent) {";
  s << "ASSERT(" << name_ << "::IsInstance(*parent));";
  s << "return static_cast<" << name_ << "*>(parent);";
  s << "}";
}

void StructDef::GenToString(std::ostream& s) const {
  s << "std::string ToString() const {";
  s << "std::stringstream ss;";
  s << "ss << std::hex << std::showbase << \"" << name_ << " { \";";

  if (fields_.size() > 0) {
    s << "ss";
    bool firstfield = true;
    for (const auto& field : fields_) {
      if (field->GetFieldType() == ReservedField::kFieldType ||
          field->GetFieldType() == ChecksumStartField::kFieldType ||
          field->GetFieldType() == FixedScalarField::kFieldType || field->GetFieldType() == CountField::kFieldType ||
          field->GetFieldType() == SizeField::kFieldType)
        continue;

      s << (firstfield ? " << \"" : " << \", ") << field->GetName() << " = \" << ";

      field->GenStringRepresentation(s, field->GetName() + "_");

      if (firstfield) {
        firstfield = false;
      }
    }
    s << ";";
  }

  s << "ss << \" }\";";
  s << "return ss.str();";
  s << "}\n";
}

void StructDef::GenParse(std::ostream& s) const {
  std::string iterator = (is_little_endian_ ? "Iterator<kLittleEndian>" : "Iterator<!kLittleEndian>");

  if (fields_.HasBody()) {
    s << "static std::optional<" << iterator << ">";
  } else {
    s << "static " << iterator;
  }

  s << " Parse(" << name_ << "* to_fill, " << iterator << " struct_begin_it ";

  if (parent_ != nullptr) {
    s << ", bool fill_parent = true) {";
  } else {
    s << ") {";
  }
  s << "auto to_bound = struct_begin_it;";

  if (parent_ != nullptr) {
    s << "if (fill_parent) {";
    std::string parent_param = (parent_->parent_ == nullptr ? "" : ", true");
    if (parent_->fields_.HasBody()) {
      s << "auto parent_optional_it = " << parent_->name_ << "::Parse(to_fill, to_bound" << parent_param << ");";
      if (fields_.HasBody()) {
        s << "if (!parent_optional_it) { return {}; }";
      } else {
        s << "ASSERT(parent_optional_it.has_value());";
      }
    } else {
      s << parent_->name_ << "::Parse(to_fill, to_bound" << parent_param << ");";
    }
    s << "}";
  }

  if (!fields_.HasBody()) {
    s << "size_t end_index = struct_begin_it.NumBytesRemaining();";
    s << "if (end_index < " << GetSize().bytes() << ")";
    s << "{ return struct_begin_it.Subrange(0,0);}";
  }

  Size total_bits{0};
  for (const auto& field : fields_) {
    if (field->GetFieldType() != ReservedField::kFieldType && field->GetFieldType() != BodyField::kFieldType &&
        field->GetFieldType() != FixedScalarField::kFieldType &&
        field->GetFieldType() != ChecksumStartField::kFieldType && field->GetFieldType() != ChecksumField::kFieldType &&
        field->GetFieldType() != CountField::kFieldType) {
      total_bits += field->GetSize().bits();
    }
  }
  s << "{";
  s << "if (to_bound.NumBytesRemaining() < " << total_bits.bytes() << ")";
  if (!fields_.HasBody()) {
    s << "{ return to_bound.Subrange(to_bound.NumBytesRemaining(),0);}";
  } else {
    s << "{ return {};}";
  }
  s << "}";
  for (const auto& field : fields_) {
    if (field->GetFieldType() != ReservedField::kFieldType && field->GetFieldType() != BodyField::kFieldType &&
        field->GetFieldType() != FixedScalarField::kFieldType && field->GetFieldType() != SizeField::kFieldType &&
        field->GetFieldType() != ChecksumStartField::kFieldType && field->GetFieldType() != ChecksumField::kFieldType &&
        field->GetFieldType() != CountField::kFieldType) {
      s << "{";
      int num_leading_bits =
          field->GenBounds(s, GetStructOffsetForField(field->GetName()), Size(), field->GetStructSize());
      s << "auto " << field->GetName() << "_ptr = &to_fill->" << field->GetName() << "_;";
      field->GenExtractor(s, num_leading_bits, true);
      s << "}";
    }
    if (field->GetFieldType() == CountField::kFieldType || field->GetFieldType() == SizeField::kFieldType) {
      s << "{";
      int num_leading_bits =
          field->GenBounds(s, GetStructOffsetForField(field->GetName()), Size(), field->GetStructSize());
      s << "auto " << field->GetName() << "_ptr = &to_fill->" << field->GetName() << "_extracted_;";
      field->GenExtractor(s, num_leading_bits, true);
      s << "}";
    }
  }
  s << "return struct_begin_it + to_fill->size();";
  s << "}";
}

void StructDef::GenParseFunctionPrototype(std::ostream& s) const {
  s << "std::unique_ptr<" << name_ << "> Parse" << name_ << "(";
  if (is_little_endian_) {
    s << "Iterator<kLittleEndian>";
  } else {
    s << "Iterator<!kLittleEndian>";
  }
  s << "it);";
}

void StructDef::GenDefinition(std::ostream& s) const {
  s << "class " << name_;
  if (parent_ != nullptr) {
    s << " : public " << parent_->name_;
  } else {
    if (is_little_endian_) {
      s << " : public PacketStruct<kLittleEndian>";
    } else {
      s << " : public PacketStruct<!kLittleEndian>";
    }
  }
  s << " {";
  s << " public:";

  GenDefaultConstructor(s);
  GenConstructor(s);

  s << " public:\n";
  s << "  virtual ~" << name_ << "() = default;\n";

  GenSerialize(s);
  s << "\n";

  GenParse(s);
  s << "\n";

  GenSize(s);
  s << "\n";

  GenInstanceOf(s);
  s << "\n";

  GenSpecialize(s);
  s << "\n";

  GenToString(s);
  s << "\n";

  GenMembers(s);
  for (const auto& field : fields_) {
    if (field->GetFieldType() == CountField::kFieldType || field->GetFieldType() == SizeField::kFieldType) {
      s << "\n private:\n";
      s << " mutable " << field->GetDataType() << " " << field->GetName() << "_extracted_{0};";
    }
  }
  s << "};\n";

  if (fields_.HasBody()) {
    GenParseFunctionPrototype(s);
  }
  s << "\n";
}

void StructDef::GenDefinitionPybind11(std::ostream& s) const {
  s << "py::class_<" << name_;
  if (parent_ != nullptr) {
    s << ", " << parent_->name_;
  } else {
    if (is_little_endian_) {
      s << ", PacketStruct<kLittleEndian>";
    } else {
      s << ", PacketStruct<!kLittleEndian>";
    }
  }
  s << ", std::shared_ptr<" << name_ << ">";
  s << ">(m, \"" << name_ << "\")";
  s << ".def(py::init<>())";
  s << ".def(\"Serialize\", [](" << GetTypeName() << "& obj){";
  s << "std::vector<uint8_t> bytes;";
  s << "BitInserter bi(bytes);";
  s << "obj.Serialize(bi);";
  s << "return bytes;})";
  s << ".def(\"Parse\", &" << name_ << "::Parse)";
  s << ".def(\"size\", &" << name_ << "::size)";
  for (const auto& field : fields_) {
    if (field->GetBuilderParameterType().empty()) {
      continue;
    }
    s << ".def_readwrite(\"" << field->GetName() << "\", &" << name_ << "::" << field->GetName() << "_)";
  }
  s << ";\n";
}

// Generate constructor which provides default values for all struct fields.
void StructDef::GenDefaultConstructor(std::ostream& s) const {
  if (parent_ != nullptr) {
    s << name_ << "(const " << parent_->name_ << "& parent) : " << parent_->name_ << "(parent) {}";
    s << name_ << "() : " << parent_->name_ << "() {";
  } else {
    s << name_ << "() {";
  }

  // Get the list of parent params.
  FieldList parent_params;
  if (parent_ != nullptr) {
    parent_params = parent_->GetParamList().GetFieldsWithoutTypes({
        PayloadField::kFieldType,
        BodyField::kFieldType,
    });

    // Set constrained parent fields to their correct values.
    for (const auto& field : parent_params) {
      const auto& constraint = parent_constraints_.find(field->GetName());
      if (constraint != parent_constraints_.end()) {
        s << parent_->name_ << "::" << field->GetName() << "_ = ";
        if (field->GetFieldType() == ScalarField::kFieldType) {
          s << std::get<int64_t>(constraint->second) << ";";
        } else if (field->GetFieldType() == EnumField::kFieldType) {
          s << std::get<std::string>(constraint->second) << ";";
        } else {
          ERROR(field) << "Constraints on non enum/scalar fields should be impossible.";
        }
      }
    }
  }

  s << "}\n";
}

// Generate constructor which inputs initial field values for all struct fields.
void StructDef::GenConstructor(std::ostream& s) const {
  // Fetch the list of parameters and parent paremeters.
  // GetParamList returns the list of all inherited fields that do not
  // have a constrained value.
  FieldList parent_params;
  FieldList params = GetParamList().GetFieldsWithoutTypes({
      PayloadField::kFieldType,
      BodyField::kFieldType,
  });

  if (parent_ != nullptr) {
    parent_params = parent_->GetParamList().GetFieldsWithoutTypes({
        PayloadField::kFieldType,
        BodyField::kFieldType,
    });
  }

  // Generate constructor parameters for struct fields.
  s << name_ << "(";
  bool add_comma = false;
  for (auto const& field : params) {
    if (add_comma) {
      s << ", ";
    }
    field->GenBuilderParameter(s);
    add_comma = true;
  }

  s << ")" << std::endl;

  if (params.size() > 0) {
    s << " : ";
  }

  // Invoke parent constructor with correct field values.
  if (parent_ != nullptr) {
    s << parent_->name_ << "(";
    add_comma = false;
    for (auto const& field : parent_params) {
      if (add_comma) {
        s << ", ";
      }

      // Check for fields with constraint value.
      const auto& constraint = parent_constraints_.find(field->GetName());
      if (constraint != parent_constraints_.end()) {
        s << "/* " << field->GetName() << " */ ";
        if (field->GetFieldType() == ScalarField::kFieldType) {
          s << std::get<int64_t>(constraint->second);
        } else if (field->GetFieldType() == EnumField::kFieldType) {
          s << std::get<std::string>(constraint->second);
        } else {
          ERROR(field) << "Constraints on non enum/scalar fields should be impossible.";
        }
      } else if (field->BuilderParameterMustBeMoved()) {
        s << "std::move(" << field->GetName() << ")";
      } else {
        s << field->GetName();
      }

      add_comma = true;
    }

    s << ")";
  }

  // Initialize remaining fields.
  add_comma = parent_ != nullptr;
  for (auto const& field : params) {
    if (fields_.GetField(field->GetName()) == nullptr) {
      continue;
    }
    if (add_comma) {
      s << ", ";
    }

    if (field->BuilderParameterMustBeMoved()) {
      s << field->GetName() << "_(std::move(" << field->GetName() << "))";
    } else {
      s << field->GetName() << "_(" << field->GetName() << ")";
    }

    add_comma = true;
  }

  s << std::endl;
  s << "{}\n";
}

Size StructDef::GetStructOffsetForField(std::string field_name) const {
  auto size = Size(0);
  for (auto it = fields_.begin(); it != fields_.end(); it++) {
    // We've reached the field, end the loop.
    if ((*it)->GetName() == field_name) break;
    const auto& field = *it;
    // When we need to parse this field, all previous fields should already be parsed.
    if (field->GetStructSize().empty()) {
      ERROR() << "Empty size for field " << (*it)->GetName() << " finding the offset for field: " << field_name;
    }
    size += field->GetStructSize();
  }

  // We need the offset until a body field.
  if (parent_ != nullptr) {
    auto parent_body_offset = static_cast<StructDef*>(parent_)->GetStructOffsetForField("body");
    if (parent_body_offset.empty()) {
      ERROR() << "Empty offset for body in " << parent_->name_ << " finding the offset for field: " << field_name;
    }
    size += parent_body_offset;
  }

  return size;
}
