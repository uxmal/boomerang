/*
 * Copyright (C) 1998-2001, The University of Queensland
 * Copyright (C) 2001, Sun Microsystems, Inc
 * Copyright (C) 2002, Trent Waddington
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       type.cpp
  * \brief   Implementation of the Type class: low level type information
  ******************************************************************************/
#include "type.h"

#include "types.h"
#include "util.h"
#include "exp.h"
#include "cfg.h"
#include "proc.h"
#include "signature.h"
#include "boomerang.h"
#include "log.h"

#include <QtCore/QDebug>
#include <cassert>
#include <cstring>

extern char debug_buffer[]; // For prints functions
QMap<QString, SharedType > Type::namedTypes;
//QMap<QString, SharedType > Type::namedTypes;

bool Type::isCString() {
    if (!resolvesToPointer())
        return false;
    SharedType p = asPointer()->getPointsTo();
    if (p->resolvesToChar())
        return true;
    if (!p->resolvesToArray())
        return false;
    p = p->asArray()->getBaseType();
    return p->resolvesToChar();
}

Type::Type(eType _id) : id(_id) {}

VoidType::VoidType() : Type(eVoid) {}

FuncType::FuncType(Signature *sig) : Type(eFunc), signature(sig) {}

FloatType::FloatType(int sz) : Type(eFloat), size(sz) {}
std::shared_ptr<FloatType> FloatType::get(int sz) { return std::make_shared<FloatType>(sz); }

BooleanType::BooleanType() : Type(eBoolean) {}

CharType::CharType() : Type(eChar) {}

void PointerType::setPointsTo(SharedType p) {
    if (p.get() == this) {                // Note: comparing pointers
        points_to = VoidType::get(); // Can't point to self; impossible to compare, print, etc
        if (VERBOSE)
            LOG << "Warning: attempted to create pointer to self: " << ADDRESS::host_ptr(this) << "\n";
    } else
        points_to = p;
}

PointerType::PointerType(SharedType p) : Type(ePointer) { setPointsTo(p); }
ArrayType::ArrayType(SharedType p, unsigned _length) : Type(eArray), BaseType(p), Length(_length) {}

ArrayType::ArrayType(SharedType p) : Type(eArray), BaseType(p), Length(NO_BOUND) {}

bool ArrayType::isUnbounded() const { return Length == NO_BOUND; }

size_t ArrayType::convertLength(SharedType b) const {
    // MVE: not sure if this is always the right thing to do
    if (Length != NO_BOUND) {
        size_t baseSize = BaseType->getSize() / 8; // Old base size (one element) in bytes
        if (baseSize == 0)
            baseSize = 1;   // Count void as size 1
        baseSize *= Length; // Old base size (length elements) in bytes
        size_t newSize = b->getSize() / 8;
        if (newSize == 0)
            newSize = 1;
        return baseSize / newSize; // Preserve same byte size for array
    }
    return NO_BOUND;
}
void ArrayType::setBaseType(SharedType b) {
    // MVE: not sure if this is always the right thing to do
    if (Length != NO_BOUND) {
        size_t baseSize = BaseType->getSize() / 8; // Old base size (one element) in bytes
        if (baseSize == 0)
            baseSize = 1;   // Count void as size 1
        baseSize *= Length; // Old base size (length elements) in bytes
        size_t newSize = b->getSize() / 8;
        if (newSize == 0)
            newSize = 1;
        Length = baseSize / newSize; // Preserve same byte size for array
    }
    BaseType = b;
}

NamedType::NamedType(const QString &_name) : Type(eNamed), name(_name) {}

CompoundType::CompoundType(bool is_generic /* = false */)
    : Type(eCompound), nextGenericMemberNum(1), generic(is_generic) {}

UnionType::UnionType() : Type(eUnion) {}

Type::~Type() {}
VoidType::~VoidType() {}
FuncType::~FuncType() {}
// IntegerType::~IntegerType() { }
FloatType::~FloatType() {}
BooleanType::~BooleanType() {}
CharType::~CharType() {}
PointerType::~PointerType() {
    // delete points_to;        // Easier for test code (which doesn't use garbage collection)
}
ArrayType::~ArrayType() {
    // delete base_type;
}
NamedType::~NamedType() {}
CompoundType::~CompoundType() {}
UnionType::~UnionType() {}

std::shared_ptr<IntegerType> IntegerType::get(unsigned NumBits, int sign) { return std::make_shared<IntegerType>(NumBits, sign); }
/***************************************************************************/ /**
  *
  * \brief        Deep copy of this type
  * \returns            Copy of the type
  ******************************************************************************/
SharedType IntegerType::clone() const { return IntegerType::get(size, signedness); }

SharedType FloatType::clone() const { return FloatType::get(size); }

SharedType BooleanType::clone() const {
    return std::make_shared<BooleanType>();
}

SharedType CharType::clone() const {
    return CharType::get();
}

SharedType VoidType::clone() const {
    return VoidType::get();
}

SharedType FuncType::clone() const {
    return FuncType::get(signature);
}

SharedType PointerType::clone() const {
    return PointerType::get(points_to->clone());
}

SharedType ArrayType::clone() const {
    return ArrayType::get(BaseType->clone(), Length);
}

SharedType NamedType::clone() const {
    return NamedType::get(name);
}

SharedType CompoundType::clone() const {
    auto t = CompoundType::get();
    for (unsigned i = 0; i < types.size(); i++)
        t->addType(types[i]->clone(), names[i]);
    return t;
}

SharedType UnionType::clone() const {
    auto u = std::make_shared<UnionType>();
    for (UnionElement el : li)
        u->addType(el.type, el.name);
    return u;
}

SharedType SizeType::clone() const {
    return SizeType::get(size);
}

SharedType UpperType::clone() const {
    return std::make_shared<UpperType>(base_type->clone());
}

SharedType LowerType::clone() const {
    return std::make_shared<LowerType>(base_type->clone());
}

size_t IntegerType::getSize() const { return size; }
size_t FloatType::getSize() const { return size; }
size_t BooleanType::getSize() const { return 1; }
size_t CharType::getSize() const { return 8; }
size_t VoidType::getSize() const { return 0; }
size_t FuncType::getSize() const { return 0; /* always nagged me */ }
size_t PointerType::getSize() const {
    // points_to->getSize(); // yes, it was a good idea at the time
    return STD_SIZE;
}
size_t ArrayType::getSize() const { return BaseType->getSize() * Length; }
size_t NamedType::getSize() const {
    SharedType ty = resolvesTo();
    if (ty)
        return ty->getSize();
    LOG_VERBOSE(1) << "WARNING: Unknown size for named type " << name << "\n";
    return 0; // don't know
}
size_t CompoundType::getSize() const {
    int n = 0;
    for (auto &elem : types)
        // NOTE: this assumes no padding... perhaps explicit padding will be needed
        n += elem->getSize();
    return n;
}
size_t UnionType::getSize() const {
    int max = 0;
    for (const UnionElement &elem : li) {
        int sz = elem.type->getSize();
        if (sz > max)
            max = sz;
    }
    return max;
}
size_t SizeType::getSize() const { return size; }

SharedType CompoundType::getType(const QString &nam) {
    for (unsigned i = 0; i < types.size(); i++)
        if (names[i] == nam)
            return types[i];
    return nullptr;
}

// Note: n is a BIT offset
SharedType CompoundType::getTypeAtOffset(unsigned n) {
    unsigned offset = 0;
    for (auto &elem : types) {
        if (offset <= n && n < offset + elem->getSize())
            return elem;
        offset += elem->getSize();
    }
    return nullptr;
}

// Note: n is a BIT offset
void CompoundType::setTypeAtOffset(unsigned n, SharedType ty) {
    unsigned offset = 0;
    for (unsigned i = 0; i < types.size(); i++) {
        if (offset <= n && n < offset + types[i]->getSize()) {
            unsigned oldsz = types[i]->getSize();
            types[i] = ty;
            if (ty->getSize() < oldsz) {
                types.push_back(types[types.size() - 1]);
                names.push_back(names[names.size() - 1]);
                for (size_t _n = types.size() - 1; _n > i; _n--) {
                    types[_n] = types[_n - 1];
                    names[_n] = names[_n - 1];
                }
                types[i + 1] = SizeType::get(oldsz - ty->getSize());
                names[i + 1] = "pad";
            }
            return;
        }
        offset += types[i]->getSize();
    }
}

void CompoundType::setNameAtOffset(unsigned n, const QString &nam) {
    unsigned offset = 0;
    for (unsigned i = 0; i < types.size(); i++) {
        if (offset <= n && n < offset + types[i]->getSize()) {
            names[i] = nam;
            return;
        }
        offset += types[i]->getSize();
    }
}

QString CompoundType::getNameAtOffset(size_t n) {
    unsigned offset = 0;
    for (unsigned i = 0; i < types.size(); i++) {
        // if (offset >= n && n < offset + types[i]->getSize())
        if (offset <= n && n < offset + types[i]->getSize())
            // return getName(offset == n ? i : i - 1);
            return names[i];
        offset += types[i]->getSize();
    }
    return nullptr;
}

unsigned CompoundType::getOffsetTo(unsigned n) {
    unsigned offset = 0;
    for (unsigned i = 0; i < n; i++) {
        offset += types[i]->getSize();
    }
    return offset;
}

unsigned CompoundType::getOffsetTo(const QString &member) {
    unsigned offset = 0;
    for (unsigned i = 0; i < types.size(); i++) {
        if (names[i] == member)
            return offset;
        offset += types[i]->getSize();
    }
    return (unsigned)-1;
}

unsigned CompoundType::getOffsetRemainder(unsigned n) {
    unsigned r = n;
    unsigned offset = 0;
    for (auto &elem : types) {
        offset += elem->getSize();
        if (offset > n)
            break;
        r -= elem->getSize();
    }
    return r;
}

/***************************************************************************/ /**
  *
  * \brief        static Constructor from string
  * \param        str: string to parse
  * \returns       constructed type.
  ******************************************************************************/
SharedType Type::parseType(const char * /*str*/) {
    assert(!"Not implemented");
    return nullptr;
}

/***************************************************************************/ /**
  *
  * \brief        Equality comparsion.
  * \param        other - Type being compared to
  * \returns            this == other
  ******************************************************************************/
bool IntegerType::operator==(const Type &other) const {
    if (! other.isInteger())
        return false;
    IntegerType &otherInt = (IntegerType &)other;
    return
           // Note: zero size matches any other size (wild, or unknown, size)
           (size == 0 || otherInt.size == 0 || size == otherInt.size) &&
           // Note: actual value of signedness is disregarded, just whether less than, equal to, or greater than 0
           ((signedness < 0 && otherInt.signedness < 0) || (signedness == 0 && otherInt.signedness == 0) ||
            (signedness > 0 && otherInt.signedness > 0));
}

bool FloatType::operator==(const Type &other) const {
    return other.isFloat() && (size == 0 || ((FloatType &)other).size == 0 || (size == ((FloatType &)other).size));
}

bool BooleanType::operator==(const Type &other) const { return other.isBoolean(); }

bool CharType::operator==(const Type &other) const { return other.isChar(); }

bool VoidType::operator==(const Type &other) const { return other.isVoid(); }

bool FuncType::operator==(const Type &other) const {
    if (!other.isFunc())
        return false;
    // Note: some functions don't have a signature (e.g. indirect calls that have not yet been successfully analysed)
    if (signature == nullptr)
        return ((FuncType &)other).signature == nullptr;
    return *signature == *((FuncType &)other).signature;
}

static int pointerCompareNest = 0;
bool PointerType::operator==(const Type &other) const {
    //    return other.isPointer() && (*points_to == *((PointerType&)other).points_to);
    if (!other.isPointer())
        return false;
    if (++pointerCompareNest >= 20) {
        LOG_STREAM() << "PointerType operator== nesting depth exceeded!\n";
        return true;
    }
    bool ret = (*points_to == *((PointerType &)other).points_to);
    pointerCompareNest--;
    return ret;
}

bool ArrayType::operator==(const Type &other) const {
    return other.isArray() && *BaseType == *((ArrayType &)other).BaseType && ((ArrayType &)other).Length == Length;
}

bool NamedType::operator==(const Type &other) const { return other.isNamed() && (name == ((NamedType &)other).name); }

bool CompoundType::operator==(const Type &other) const {
    const CompoundType &cother = (CompoundType &)other;
    if (other.isCompound() && cother.types.size() == types.size()) {
        for (unsigned i = 0; i < types.size(); i++)
            if (!(*types[i] == *cother.types[i]))
                return false;
        return true;
    }
    return false;
}

bool UnionType::operator==(const Type &other) const {
    if(!other.isUnion())
        return false;
    const UnionType &uother = (UnionType &)other;
    if (uother.li.size() != li.size())
        return false;
    for (const UnionElement &el : li)
        if(uother.li.find(el)==uother.li.end())
            return false;
    return true;
}

bool SizeType::operator==(const Type &other) const { return other.isSize() && (size == ((SizeType &)other).size); }
bool UpperType::operator==(const Type &other) const {
    return other.isUpper() && *base_type == *((UpperType &)other).base_type;
}

bool LowerType::operator==(const Type &other) const {
    return other.isLower() && *base_type == *((LowerType &)other).base_type;
}

/***************************************************************************/ /**
  *
  * \brief        Inequality comparsion.
  * \param        other - Type being compared to
  * \returns            this == other
  ******************************************************************************/
bool Type::operator!=(const Type &other) const { return !(*this == other); }

/***************************************************************************/ /**
  *
  * \brief        Equality operator, ignoring sign. True if equal in broad
  *                      type and size, but not necessarily sign
  *                      Considers all float types > 64 bits to be the same
  * \param        other - Type being compared to
  * \returns            this == other (ignoring sign)
  ******************************************************************************/
// bool IntegerType::operator-=(const Type& other) const {
//        if (!other.isInteger()) return false;
//        return size == ((IntegerType&)other).size;
//}

// bool FloatType::operator-=(const Type& other) const {
//        if (!other.isFloat()) return false;
//        if (size > 64 && ((FloatType&)other).size > 64)
//        return true;
//        return size == ((FloatType&)other).size;
//}

/***************************************************************************/ /**
  *
  * \brief        Defines an ordering between Type's
  *                      (and hence sets etc of Exp* using lessExpStar).
  * \param        other - Type being compared to
  * \returns            this is less than other
  ******************************************************************************/
bool IntegerType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    if (size < ((IntegerType &)other).size)
        return true;
    if (size > ((IntegerType &)other).size)
        return false;
    return (signedness < ((IntegerType &)other).signedness);
}

bool FloatType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (size < ((FloatType &)other).size);
}

bool VoidType::operator<(const Type &other) const { return id < other.getId(); }

bool FuncType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    // FIXME: Need to compare signatures
    return true;
}

bool BooleanType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return true;
}

bool CharType::operator<(const Type &other) const { return id < other.getId(); }

bool PointerType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (*points_to < *((PointerType &)other).points_to);
}

bool ArrayType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (*BaseType < *((ArrayType &)other).BaseType);
}

bool NamedType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (name < ((NamedType &)other).name);
}

bool CompoundType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return getSize() < other.getSize(); // This won't separate structs of the same size!! MVE
}

bool UnionType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return getNumTypes() < ((const UnionType &)other).getNumTypes();
}

bool SizeType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (size < ((SizeType &)other).size);
}

bool UpperType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (*base_type < *((UpperType &)other).base_type);
}

bool LowerType::operator<(const Type &other) const {
    if (id < other.getId())
        return true;
    if (id > other.getId())
        return false;
    return (*base_type < *((LowerType &)other).base_type);
}

/***************************************************************************/ /**
  *
  * \brief        Match operation.
  * \param        pattern - Type to match
  * \returns            Exp list of bindings if match or nullptr
  ******************************************************************************/
Exp *Type::match(SharedType pattern) {
    if (pattern->isNamed()) {
        LOG << "type match: " << this->getCtype() << " to " << pattern->getCtype() << "\n";
        return Binary::get(opList, Binary::get(opEquals, new Unary(opVar, new Const(pattern->asNamed()->getName())),
                                               new TypeVal(this->clone())),
                           new Terminal(opNil));
    }
    return nullptr;
}

Exp *IntegerType::match(SharedType pattern) { return Type::match(pattern); }

Exp *FloatType::match(SharedType pattern) { return Type::match(pattern); }

Exp *BooleanType::match(SharedType pattern) { return Type::match(pattern); }

Exp *CharType::match(SharedType pattern) { return Type::match(pattern); }

Exp *VoidType::match(SharedType pattern) { return Type::match(pattern); }

Exp *FuncType::match(SharedType pattern) { return Type::match(pattern); }

Exp *PointerType::match(SharedType pattern) {
    if (pattern->isPointer()) {
        LOG << "got pointer match: " << this->getCtype() << " to " << pattern->getCtype() << "\n";
        return points_to->match(pattern->asPointer()->getPointsTo());
    }
    return Type::match(pattern);
}

Exp *ArrayType::match(SharedType pattern) {
    if (pattern->isArray())
        return BaseType->match(pattern);
    return Type::match(pattern);
}

Exp *NamedType::match(SharedType pattern) { return Type::match(pattern); }

Exp *CompoundType::match(SharedType pattern) { return Type::match(pattern); }

Exp *UnionType::match(SharedType pattern) { return Type::match(pattern); }

/***************************************************************************/ /**
  *
  * \brief        Return a string representing this type
  * \param        final: if true, this is final output
  * \returns            Pointer to a constant string of char
  ******************************************************************************/
QString VoidType::getCtype(bool /*final*/) const { return "void"; }

QString FuncType::getCtype(bool final) const {
    if (signature == nullptr)
        return "void (void)";
    QString s;
    if (signature->getNumReturns() == 0)
        s += "void";
    else
        s += signature->getReturnType(0)->getCtype(final);
    s += " (";
    for (unsigned i = 0; i < signature->getNumParams(); i++) {
        if (i != 0)
            s += ", ";
        s += signature->getParamType(i)->getCtype(final);
    }
    s += ")";
    return s;
}

// As above, but split into the return and parameter parts
void FuncType::getReturnAndParam(QString &ret, QString &param) {
    if (signature == nullptr) {
        ret = "void";
        param = "(void)";
        return;
    }
    if (signature->getNumReturns() == 0)
        ret = "void";
    else
        ret = signature->getReturnType(0)->getCtype();
    QString s;
    s += " (";
    for (unsigned i = 0; i < signature->getNumParams(); i++) {
        if (i != 0)
            s += ", ";
        s += signature->getParamType(i)->getCtype();
    }
    s += ")";
    param = s;
}

QString IntegerType::getCtype(bool final) const {
    if (signedness >= 0) {
        QString s;
        if (!final && signedness == 0)
            s = "/*signed?*/";
        switch (size) {
        case 32:
            s += "int";
            break;
        case 16:
            s += "short";
            break;
        case 8:
            s += "char";
            break;
        case 1:
            s += "bool";
            break;
        case 64:
            s += "long long";
            break;
        default:
            if (!final)
                s += "?"; // To indicate invalid/unknown size
            s += "int";
        }
        return s;
    } else {
        switch (size) {
        case 32:
            return "unsigned int";
        case 16:
            return "unsigned short";
        case 8:
            return "unsigned char";
        case 1:
            return "bool";
        case 64:
            return "unsigned long long";
        default:
            if (final)
                return "unsigned int";
            else
                return "?unsigned int";
        }
    }
}

QString FloatType::getCtype(bool /*final*/) const {
    switch (size) {
    case 32:
        return "float";
    case 64:
        return "double";
    default:
        return "double";
    }
}

QString BooleanType::getCtype(bool /*final*/) const { return "bool"; }

QString CharType::getCtype(bool /*final*/) const { return "char"; }

QString PointerType::getCtype(bool final) const {
    QString s = points_to->getCtype(final);
    if (points_to->isPointer())
        s += "*";
    else
        s += " *";
    return s; // memory..
}

QString ArrayType::getCtype(bool final) const {
    QString s = BaseType->getCtype(final);
    if (isUnbounded())
        return s + "[]";
    return s + "[" + QString::number(Length) + "]";
}

QString NamedType::getCtype(bool /*final*/) const { return name; }

QString CompoundType::getCtype(bool final) const {
    QString tmp("struct { ");
    for (unsigned i = 0; i < types.size(); i++) {
        tmp += types[i]->getCtype(final);
        if (names[i] != "") {
            tmp += " ";
            tmp += names[i];
        }
        tmp += "; ";
    }
    tmp += "}";
    return tmp;
}

QString UnionType::getCtype(bool final) const {
    QString tmp("union { ");
    for (const UnionElement &el : li) {
        tmp += el.type->getCtype(final);
        if (el.name != "") {
            tmp += " ";
            tmp += el.name;
        }
        tmp += "; ";
    }
    tmp += "}";
    return tmp;
}

QString SizeType::getCtype(bool /*final*/) const {
    // Emit a comment and the size
    QString res;
    QTextStream ost(&res);
    ost << "__size" << size;
    return res;
}

QString UpperType::getCtype(bool /*final*/) const {
    QString res;
    QTextStream ost(&res);
    ost << "/*upper*/(" << base_type << ")";
    return res;
}
QString LowerType::getCtype(bool /*final*/) const {
    QString res;
    QTextStream ost(&res);
    ost << "/*lower*/(" << base_type << ")";
    return res;
}

QString Type::prints() {
    return getCtype(false); // For debugging
}

void Type::dump() {
    LOG_STREAM() << getCtype(false); // For debugging
}

// named type accessors
void Type::addNamedType(const QString &name, SharedType type) {
    if (namedTypes.find(name) != namedTypes.end()) {
        if (!(*type == *namedTypes[name])) {
            // LOG << "addNamedType: name " << name << " type " << type->getCtype() << " != " <<
            //    namedTypes[name]->getCtype() << "\n";// << std::flush;
            // LOGTAIL;
            qWarning() << "Warning: Type::addNamedType: Redefinition of type " << name << "\n";
            qWarning() << " type     = " << type->prints() << "\n";
            qWarning() << " previous = " << namedTypes[name]->prints() << "\n";
            namedTypes[name] = type; // WARN: was *type==*namedTypes[name], verify !
        }
    } else {
        // check if it is:
        // typedef int a;
        // typedef a b;
        // we then need to define b as int
        // we create clones to keep the GC happy
        if (namedTypes.find(type->getCtype()) != namedTypes.end()) {
            namedTypes[name] = namedTypes[type->getCtype()]->clone();
        } else {
            namedTypes[name] = type->clone();
        }
    }
}

SharedType Type::getNamedType(const QString &name) {
    auto iter= namedTypes.find(name);
    if (iter == namedTypes.end())
        return nullptr;
    return *iter;
}

void Type::dumpNames() {
    for (auto it = namedTypes.begin(); it != namedTypes.end(); ++it)
        qDebug() << it.key() << " -> " << it.value()->getCtype() << "\n";
}

/***************************************************************************/ /**
  *
  * \brief   Given the name of a temporary variable, return its Type
  * \param   name reference to a string (e.g. "tmp", "tmpd")
  * \returns       Ptr to a new Type object
  ******************************************************************************/
SharedType Type::getTempType(const QString &name) {
    SharedType ty;
    QChar ctype = ' ';
    if (name.size() > 3)
        ctype = name[3];
    switch (ctype.toLatin1()) {
    // They are all int32, except for a few specials
    case 'f':
        ty = FloatType::get(32);
        break;
    case 'd':
        ty = FloatType::get(64);
        break;
    case 'F':
        ty = FloatType::get(80);
        break;
    case 'D':
        ty = FloatType::get(128);
        break;
    case 'l':
        ty = IntegerType::get(64);
        break;
    case 'h':
        ty = IntegerType::get(16);
        break;
    case 'b':
        ty = IntegerType::get(8);
        break;
    default:
        ty = IntegerType::get(32);
        break;
    }
    return ty;
}

/***************************************************************************/ /**
  *
  * \brief   Return a minimal temporary name for this type. It'd be even
  *          nicer to return a unique name, but we don't know scope at
  *          this point, and even so we could still clash with a user-defined
  *          name later on :(
  * \returns        a string
  ******************************************************************************/
QString IntegerType::getTempName() const {
    switch (size) {
    case 1: /* Treat as a tmpb */
    case 8:
        return "tmpb";
    case 16:
        return "tmph";
    case 32:
        return "tmpi";
    case 64:
        return "tmpl";
    }
    return "tmp";
}

QString FloatType::getTempName() const {
    switch (size) {
    case 32:
        return "tmpf";
    case 64:
        return "tmpd";
    case 80:
        return "tmpF";
    case 128:
        return "tmpD";
    }
    return "tmp";
}

QString Type::getTempName() const {
    return "tmp"; // what else can we do? (besides panic)
}

void Type::clearNamedTypes() { namedTypes.clear(); }

int NamedType::nextAlpha = 0;
std::shared_ptr<NamedType> NamedType::getAlpha() {
    return NamedType::get(QString("alpha%1").arg(nextAlpha++));
}

std::shared_ptr<PointerType> PointerType::newPtrAlpha() { return PointerType::get(NamedType::getAlpha()); }

// Note: alpha is therefore a "reserved name" for types
bool PointerType::pointsToAlpha() const {
    // void* counts as alpha* (and may replace it soon)
    if (points_to->isVoid())
        return true;
    if (!points_to->isNamed())
        return false;
    return points_to->asNamed()->getName().startsWith("alpha");
}

int PointerType::pointerDepth() const {
    int d = 1;
    auto pt = points_to;
    while (pt->isPointer()) {
        pt = pt->asPointer()->getPointsTo();
        d++;
    }
    return d;
}

SharedType PointerType::getFinalPointsTo() const {
    SharedType pt = points_to;
    while (pt->isPointer()) {
        pt = pt->asPointer()->getPointsTo();
    }
    return pt;
}

SharedType NamedType::resolvesTo() const {
    SharedType ty = getNamedType(name);
    if (ty && ty->isNamed())
        return std::static_pointer_cast<NamedType>(ty)->resolvesTo();
    return ty;
}

void ArrayType::fixBaseType(SharedType b) {
    if (BaseType == nullptr)
        BaseType = b;
    else {
        assert(BaseType->isArray());
        BaseType->asArray()->fixBaseType(b);
    }
}

#define AS_TYPE(x)                                                                                                     \
    std::shared_ptr<x##Type> Type::as##x() {                                                                                           \
        SharedType ty = shared_from_this();                                                                                               \
        if (isNamed())                                                                                                 \
            ty = std::static_pointer_cast<NamedType>(ty)->resolvesTo();                                                                      \
        auto res = std::dynamic_pointer_cast<x##Type>(ty);                                                         \
        assert(res);                                                                                                   \
        return res;                                                                                                    \
    }                                                                                                                  \
    std::shared_ptr<const x##Type> Type::as##x() const {                                                                               \
        auto ty = shared_from_this();                                                                                         \
        if (isNamed())                                                                                                 \
            ty = std::static_pointer_cast<const NamedType>(ty)->resolvesTo();                                                               \
        auto res = std::dynamic_pointer_cast<const x##Type>(ty);                                             \
        assert(res);                                                                                                   \
        return res;                                                                                                    \
    }

AS_TYPE(Void)
AS_TYPE(Func)
AS_TYPE(Boolean)
AS_TYPE(Char)
AS_TYPE(Integer)
AS_TYPE(Float)
AS_TYPE(Pointer)
AS_TYPE(Array)
AS_TYPE(Compound)
AS_TYPE(Size)
AS_TYPE(Union)
AS_TYPE(Upper)
AS_TYPE(Lower)
// Note: don't want to call this->resolve() for this case, since then we (probably) won't have a NamedType and the
// assert will fail
std::shared_ptr<NamedType> Type::asNamed() {
    SharedType ty = shared_from_this();
    auto res = std::dynamic_pointer_cast<NamedType>(ty);
    assert(res);
    return res;
}
std::shared_ptr<const NamedType> Type::asNamed() const {
    auto ty = shared_from_this();
    auto res = std::dynamic_pointer_cast<const NamedType>(ty);
    assert(res);
    return res;
}

#define RESOLVES_TO_TYPE(x)                                                                                            \
    bool Type::resolvesTo##x() const {                                                                                 \
        auto ty = shared_from_this();                                                                                         \
        if (ty->isNamed())                                                                                             \
            ty = std::static_pointer_cast<const NamedType>(ty)->resolvesTo();                                                                      \
        return ty && ty->is##x();                                                                                      \
    }

RESOLVES_TO_TYPE(Void)
RESOLVES_TO_TYPE(Func)
RESOLVES_TO_TYPE(Boolean)
RESOLVES_TO_TYPE(Char)
RESOLVES_TO_TYPE(Integer)
RESOLVES_TO_TYPE(Float)
RESOLVES_TO_TYPE(Pointer)
RESOLVES_TO_TYPE(Array)
RESOLVES_TO_TYPE(Compound)
RESOLVES_TO_TYPE(Union)
RESOLVES_TO_TYPE(Size)
RESOLVES_TO_TYPE(Upper)
RESOLVES_TO_TYPE(Lower)

bool Type::isPointerToAlpha() { return isPointer() && asPointer()->pointsToAlpha(); }
//! Print in *i32* format
void Type::starPrint(QTextStream &os) { os << "*" << this << "*"; }

QString Type::toString() const {
    QString res;
    QTextStream tgt(&res);
    tgt << *this;
    return res;
}

// A crude shortcut representation of a type
QTextStream &operator<<(QTextStream &os, const Type &t) {
    switch (t.getId()) {
    case eInteger: {
        int sg = t.as<IntegerType>()->getSignedness();
        // 'j' for either i or u, don't know which
        os << (sg == 0 ? 'j' : sg > 0 ? 'i' : 'u');
        os << t.asInteger()->getSize();
        break;
    }
    case eFloat:
        os << 'f';
        os << t.asFloat()->getSize();
        break;
    case ePointer:
        os << t.asPointer()->getPointsTo() << '*';
        break;
    case eSize:
        os << t.getSize();
        break;
    case eChar:
        os << 'c';
        break;
    case eVoid:
        os << 'v';
        break;
    case eBoolean:
        os << 'b';
        break;
    case eCompound:
        os << "struct";
        break;
    case eUnion:
        os << "union";
        break;
    // case eUnion:    os << t.getCtype(); break;
    case eFunc:
        os << "func";
        break;
    case eArray:
        os << '[' << t.asArray()->getBaseType();
        if (!t.asArray()->isUnbounded())
            os << ", " << t.asArray()->getLength();
        os << ']';
        break;
    case eNamed:
        os << t.asNamed()->getName();
        break;
    case eUpper:
        os << "U(" << t.asUpper()->getBaseType() << ')';
        break;
    case eLower:
        os << "L(" << t.asLower()->getBaseType() << ')';
        break;
    }
    return os;
}

QTextStream &operator<<(QTextStream &os, const SharedConstType &t) {
    if (t == nullptr)
        return os << '0';
    return os << *t;
}

// FIXME: aren't mergeWith and meetWith really the same thing?
// Merge this IntegerType with another
SharedType IntegerType::mergeWith(SharedType other) const {
    if (*this == *other)
        return ((IntegerType *)this)->shared_from_this();
    if (!other->isInteger())
        return nullptr; // Can you merge with a pointer?
    auto oth = std::static_pointer_cast<IntegerType>(other);
    auto ret = std::static_pointer_cast<IntegerType>(this->clone());
    if (size == 0)
        ret->setSize(oth->getSize());
    if (signedness == 0)
        ret->setSigned(oth->getSignedness());
    return ret;
}

// Merge this SizeType with another type
SharedType SizeType::mergeWith(SharedType other) const {
    SharedType ret = other->clone();
    ret->setSize(size);
    return ret;
}

SharedType UpperType::mergeWith(SharedType /*other*/) const {
    // FIXME: TBC
    return ((UpperType *)this)->shared_from_this();
}

SharedType LowerType::mergeWith(SharedType /*other*/) const {
    // FIXME: TBC
    return ((LowerType *)this)->shared_from_this();
}

// Return true if this is a superstructure of other, i.e. we have the same types at the same offsets as other
bool CompoundType::isSuperStructOf(const SharedType &other) {
    if (!other->isCompound())
        return false;
    auto otherCmp = other->asCompound();
    size_t n = otherCmp->types.size();
    if (n > types.size())
        return false;
    for (unsigned i = 0; i < n; i++)
        if (otherCmp->types[i] != types[i])
            return false;
    return true;
}

// Return true if this is a substructure of other, i.e. other has the same types at the same offsets as this
bool CompoundType::isSubStructOf(SharedType other) const {
    if (!other->isCompound())
        return false;
    auto otherCmp = other->asCompound();
    unsigned n = types.size();
    if (n > otherCmp->types.size())
        return false;
    for (unsigned i = 0; i < n; i++)
        if (otherCmp->types[i] != types[i])
            return false;
    return true;
}

// Return true if this type is already in the union. Note: linear search, but number of types is usually small
bool UnionType::findType(SharedType ty) {
    UnionElement ue;
    ue.type = ty;
    return li.find(ue)!=li.end();
}

void UpperType::setSize(size_t /*size*/) {
    // Does this make sense?
    assert(0);
}

void LowerType::setSize(size_t /*size*/) {
    // Does this make sense?
    assert(0);
}

SharedType Type::newIntegerLikeType(int size, int signedness) {
    if (size == 1)
        return BooleanType::get();
    if (size == 8 && signedness >= 0)
        return CharType::get();
    return IntegerType::get(size, signedness);
}

// Find the entry that overlaps with addr. If none, return end(). We have to use upper_bound and decrement the iterator,
// because we might want an entry that starts earlier than addr yet still overlaps it
DataIntervalMap::iterator DataIntervalMap::find_it(ADDRESS addr) {
    iterator it = dimap.upper_bound(addr); // Find the first item strictly greater than addr
    if (it == dimap.begin()) {
        return dimap.end(); // None <= this address, so no overlap possible
    }
    it--;                   // If any item overlaps, it is this one
    if ((addr >= it->first) && (addr - it->first).m_value < it->second.size)
        // This is the one that overlaps with addr
        return it;
    return dimap.end();
}

DataIntervalEntry *DataIntervalMap::find(ADDRESS addr) {
    iterator it = find_it(addr);
    if (it == dimap.end())
        return nullptr;
    return &*it;
}

bool DataIntervalMap::isClear(ADDRESS addr, unsigned size) {
    iterator it = dimap.upper_bound(addr + size - 1); // Find the first item strictly greater than address of last byte
    if (it == dimap.begin())
        return true; // None <= this address, so no overlap possible
    it--;            // If any item overlaps, it is this one
    // Make sure the previous item ends before this one will start
    ADDRESS end;
    if (it->first + it->second.size < it->first)
        // overflow
        end = 0xFFFFFFFF; // Overflow
    else
        end = it->first + it->second.size;
    if (end <= addr)
        return true;
    if (it->second.type->isArray() && it->second.type->asArray()->isUnbounded()) {
        it->second.size = (addr - it->first).m_value;
        LOG << "shrinking size of unbound array to " << it->second.size << " bytes\n";
        return true;
    }
    return false;
}

// With the forced parameter: are we forcing the name, the type, or always both?
//! Add a new data item
void DataIntervalMap::addItem(ADDRESS addr, QString name, SharedType ty, bool forced /* = false */) {
    if (name.isNull())
        name = "<noname>";

    DataIntervalEntry *pdie = find(addr);
    if (pdie == nullptr) {
        // Check that this new item is compatible with any items it overlaps with, and insert it
        replaceComponents(addr, name, ty, forced);
        return;
    }
    // There are two basic cases, and an error if the two data types weave
    if (pdie->first < addr) {
        // The existing entry comes first. Make sure it ends last (possibly equal last)
        if (pdie->first + pdie->second.size < addr + ty->getSize() / 8) {
            LOG << "TYPE ERROR: attempt to insert item " << name << " at " << addr << " of type " << ty->getCtype()
                << " which weaves after " << pdie->second.name << " at " << pdie->first << " of type "
                << pdie->second.type->getCtype() << "\n";
            return;
        }
        enterComponent(pdie, addr, name, ty, forced);
    } else if (pdie->first == addr) {
        // Could go either way, depending on where the data items end
        ADDRESS endOfCurrent = pdie->first + pdie->second.size;
        ADDRESS endOfNew = addr + ty->getSize() / 8;
        if (endOfCurrent < endOfNew)
            replaceComponents(addr, name, ty, forced);
        else if (endOfCurrent == endOfNew)
            checkMatching(pdie, addr, name, ty, forced); // Size match; check that new type matches old
        else
            enterComponent(pdie, addr, name, ty, forced);
    } else {
        // Old starts after new; check it also ends first
        if (pdie->first + pdie->second.size > addr + ty->getSize() / 8) {
            LOG << "TYPE ERROR: attempt to insert item " << name << " at " << addr << " of type " << ty->getCtype()
                << " which weaves before " << pdie->second.name << " at " << pdie->first << " of type "
                << pdie->second.type->getCtype() << "\n";
            return;
        }
        replaceComponents(addr, name, ty, forced);
    }
}

// We are entering an item that already exists in a larger type. Check for compatibility, meet if necessary.
void DataIntervalMap::enterComponent(DataIntervalEntry *pdie, ADDRESS addr, const QString & /*name*/, SharedType ty,
                                     bool /*forced*/) {
    if (pdie->second.type->resolvesToCompound()) {
        unsigned bitOffset = (addr - pdie->first).m_value * 8;
        SharedType memberType = pdie->second.type->asCompound()->getTypeAtOffset(bitOffset);
        if (memberType->isCompatibleWith(*ty)) {
            bool ch;
            memberType = memberType->meetWith(ty, ch);
            pdie->second.type->asCompound()->setTypeAtOffset(bitOffset, memberType);
        } else
            LOG << "TYPE ERROR: At address " << addr << " type " << ty->getCtype()
                << " is not compatible with existing structure member type "
                << memberType->getCtype() << "\n";
    } else if (pdie->second.type->resolvesToArray()) {
        SharedType memberType = pdie->second.type->asArray()->getBaseType();
        if (memberType->isCompatibleWith(*ty)) {
            bool ch;
            memberType = memberType->meetWith(ty, ch);
            pdie->second.type->asArray()->setBaseType(memberType);
        } else
            LOG << "TYPE ERROR: At address " << addr << " type " << ty->getCtype()
                << " is not compatible with existing array member type "
                << memberType->getCtype() << "\n";
    } else
        LOG << "TYPE ERROR: Existing type at address " << pdie->first << " is not structure or array type\n";
}

// We are entering a struct or array that overlaps existing components. Check for compatibility, and move the
// components out of the way, meeting if necessary
void DataIntervalMap::replaceComponents(ADDRESS addr, const QString &name, SharedType ty, bool /*forced*/) {
    iterator it;
    ADDRESS pastLast = addr + ty->getSize() / 8; // This is the byte address just past the type to be inserted
    // First check that the new entry will be compatible with everything it will overlap
    if (ty->resolvesToCompound()) {
        iterator it1 = dimap.lower_bound(addr); // Iterator to the first overlapping item (could be end(), but
        // if so, it2 will also be end())
        iterator it2 = dimap.upper_bound(pastLast - 1); // Iterator to the first item that starts too late
        for (it = it1; it != it2; ++it) {
            unsigned bitOffset = (it->first - addr).m_value * 8;

            SharedType memberType = ty->asCompound()->getTypeAtOffset(bitOffset);
            if (memberType->isCompatibleWith(*it->second.type, true)) {
                bool ch;
                qDebug() << prints();
                qDebug() << memberType->getCtype()<<" "<< it->second.type->getCtype();
                memberType = it->second.type->meetWith(memberType, ch);
                ty->asCompound()->setTypeAtOffset(bitOffset, memberType);
            } else {
                LOG << "TYPE ERROR: At address " << addr << " struct type " << ty->getCtype() <<
                       " is not compatible with existing type " << it->second.type->getCtype() << "\n";
                return;
            }
        }
    } else if (ty->resolvesToArray()) {
        SharedType memberType = ty->asArray()->getBaseType();
        iterator it1 = dimap.lower_bound(addr);
        iterator it2 = dimap.upper_bound(pastLast - 1);
        for (it = it1; it != it2; ++it) {
            if (memberType->isCompatibleWith(*it->second.type, true)) {
                bool ch;
                memberType = memberType->meetWith(it->second.type, ch);
                ty->asArray()->setBaseType(memberType);
            } else {
                LOG << "TYPE ERROR: At address " << addr << " array type " << ty->getCtype() <<
                       " is not compatible with existing type " << it->second.type->getCtype() << "\n";
                return;
            }
        }
    } else {
        // Just make sure it doesn't overlap anything
        if (!isClear(addr, (ty->getSize() + 7) / 8)) {
            LOG << "TYPE ERROR: at address " << addr << ", overlapping type " << ty->getCtype()
                << " does not resolve to compound or array\n";
            return;
        }
    }

    // The compound or array type is compatible. Remove the items that it will overlap with
    iterator it1 = dimap.lower_bound(addr);
    iterator it2 = dimap.upper_bound(pastLast - 1);

    // Check for existing locals that need to be updated
    if (ty->resolvesToCompound() || ty->resolvesToArray()) {
        Exp *rsp = Location::regOf(proc->getSignature()->getStackRegister());
        RefExp *rsp0 = new RefExp(rsp, proc->getCFG()->findTheImplicitAssign(rsp)); // sp{0}
        for (it = it1; it != it2; ++it) {
            // Check if there is an existing local here
            Exp *locl = Location::memOf(Binary::get(opPlus, rsp0->clone(), new Const(it->first.native())));
            locl->simplifyArith(); // Convert m[sp{0} + -4] to m[sp{0} - 4]
            SharedType elemTy;
            int bitOffset = (it->first - addr).m_value / 8;
            if (ty->resolvesToCompound())
                elemTy = ty->asCompound()->getTypeAtOffset(bitOffset);
            else
                elemTy = ty->asArray()->getBaseType();
            QString locName = proc->findLocal(*locl, elemTy);
            if (!locName.isNull() && ty->resolvesToCompound()) {
                auto c = ty->asCompound();
                // want s.m where s is the new compound object and m is the member at offset bitOffset
                QString memName = c->getNameAtOffset(bitOffset);
                Exp *s = Location::memOf(Binary::get(opPlus, rsp0->clone(), new Const(addr)));
                s->simplifyArith();
                Exp *memberExp = Binary::get(opMemberAccess, s, new Const(memName));
                proc->mapSymbolTo(locl, memberExp);
            } else {
                // FIXME: to be completed
            }
        }
    }

    for (it = it1; it != it2 && it != dimap.end();)
        // I believe that it is a conforming extension for map::erase() to return the iterator, but it is not portable
        // to use it. In particular, gcc considers using the return value as an error
        // The postincrement operator seems to be the definitive way to do this
        dimap.erase(it++);

    DataInterval *pdi = &dimap[addr]; // Finally add the new entry
    pdi->size = ty->getBytes();
    pdi->name = name;
    pdi->type = ty;
}

void DataIntervalMap::checkMatching(DataIntervalEntry *pdie, ADDRESS addr, const QString &/*name*/, SharedType ty,
                                    bool /*forced*/) {
    if (pdie->second.type->isCompatibleWith(*ty)) {
        // Just merge the types and exit
        bool ch=false;
        pdie->second.type = pdie->second.type->meetWith(ty, ch);
        return;
    }
    LOG << "TYPE DIFFERENCE (could be OK): At address " << addr << " existing type " << pdie->second.type->getCtype()
        << " but added type " << ty->getCtype() << "\n";
}

void DataIntervalMap::deleteItem(ADDRESS addr) {
    iterator it = dimap.find(addr);
    if (it == dimap.end())
        return;
    dimap.erase(it);
}

void DataIntervalMap::dump() { LOG_STREAM() << prints(); }

char *DataIntervalMap::prints() {
    QString tgt;
    QTextStream ost(&tgt);
    iterator it;
    for (it = dimap.begin(); it != dimap.end(); ++it)
        ost << "0x" << it->first << "-0x" << it->first+it->second.type->getBytes() << " " << it->second.name << " " << it->second.type->getCtype()
            << "\n";
    strncpy(debug_buffer, qPrintable(tgt), DEBUG_BUFSIZE - 1);
    debug_buffer[DEBUG_BUFSIZE - 1] = '\0';
    return debug_buffer;
}

ComplexTypeCompList &Type::compForAddress(ADDRESS addr, DataIntervalMap &dim) {
    DataIntervalEntry *pdie = dim.find(addr);
    ComplexTypeCompList *res = new ComplexTypeCompList;
    if (pdie == nullptr)
        return *res;
    ADDRESS startCurrent = pdie->first;
    SharedType curType = pdie->second.type;
    while (startCurrent < addr) {
        size_t bitOffset = (addr - startCurrent).m_value * 8;
        if (curType->isCompound()) {
            auto compCurType = curType->asCompound();
            unsigned rem = compCurType->getOffsetRemainder(bitOffset);
            startCurrent = addr - (rem / 8);
            ComplexTypeComp ctc;
            ctc.isArray = false;
            ctc.u.memberName = compCurType->getNameAtOffset(bitOffset);
            res->push_back(ctc);
            curType = compCurType->getTypeAtOffset(bitOffset);
        } else if (curType->isArray()) {
            curType = curType->asArray()->getBaseType();
            unsigned baseSize = curType->getSize();
            unsigned index = bitOffset / baseSize;
            startCurrent += index * baseSize / 8;
            ComplexTypeComp ctc;
            ctc.isArray = true;
            ctc.u.index = index;
            res->push_back(ctc);
        } else {
            LOG << "TYPE ERROR: no struct or array at byte address " << addr << "\n";
            return *res;
        }
    }
    return *res;
}

void UnionType::addType(SharedType n, const QString &str) {
    if (n->isUnion()) {
        auto utp = std::static_pointer_cast<UnionType>(n);
        // Note: need to check for name clashes eventually
        li.insert(utp->li.begin(), utp->li.end());
    } else {
        if (n->isPointer() && n->asPointer()->getPointsTo().get() == this) { // Note: pointer comparison
            n = PointerType::get(VoidType::get());
            LOG_VERBOSE(1) << "Warning: attempt to union with pointer to self!\n";
        }
        UnionElement ue;
        ue.type = n;
        ue.name = str;
        li.insert(ue);
    }
}

// Update this compound to use the fact that offset off has type ty
void CompoundType::updateGenericMember(int off, SharedType ty, bool &ch) {
    assert(generic);
    int bit_offset = off * 8;
    SharedType existingType = getTypeAtOffset(bit_offset);
    if (existingType) {
        existingType = existingType->meetWith(ty, ch);
        setTypeAtOffset(bit_offset, existingType);
    } else {
        QString nam = QString("member")+ QString::number(nextGenericMemberNum++);
        setTypeAtOffset(bit_offset, ty);
        setNameAtOffset(bit_offset, nam);
    }
}

