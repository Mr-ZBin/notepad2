// This file is part of Notepad2.
// See License.txt for details about distribution and modification.
//! Lexer for Java, Android IDL, BeanShell.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>
#include <vector>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "StringUtils.h"
#include "LexerModule.h"
#include "LexerUtils.h"

using namespace Lexilla;

namespace {

struct EscapeSequence {
	int outerState = SCE_JAVA_DEFAULT;
	int digitsLeft = 0;
	bool hex = false;

	// highlight any character as escape sequence.
	bool resetEscapeState(int state, int chNext) noexcept {
		if (IsEOLChar(chNext)) {
			return false;
		}
		outerState = state;
		digitsLeft = 1;
		if (chNext == 'u') {
			digitsLeft = 5;
			hex = true;
		} else if (IsOctalDigit(chNext)) {
			digitsLeft = 3;
			hex = false;
		}
		return true;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsOctalOrHex(ch, hex);
	}
};

enum {
	JavaLineStateMaskLineComment = 1, // line comment
	JavaLineStateMaskImport = 1 << 1, // import
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Type = 1,
	KeywordIndex_Directive = 2,
	KeywordIndex_Class = 3,
	KeywordIndex_Interface = 4,
	KeywordIndex_Enumeration = 5,
	KeywordIndex_Constant = 6,
	KeywordIndex_Function = 8,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

enum class DocTagState {
	None,
	At,				// @param x
	InlineAt,		// {@link package.class#member label}
	TagOpen,		// <tag>
	TagClose,		// </tag>
};

enum class KeywordType {
	None = SCE_JAVA_DEFAULT,
	Annotation = SCE_JAVA_ANNOTATION,
	Class = SCE_JAVA_CLASS,
	Interface = SCE_JAVA_INTERFACE,
	Enum = SCE_JAVA_ENUM,
	Record = SCE_JAVA_RECORD,
	Label = SCE_JAVA_LABEL,
	Return = 0x40,
	While,
};

static_assert(DefaultNestedStateBaseStyle + 1 == SCE_JAVA_TEMPLATE);
static_assert(DefaultNestedStateBaseStyle + 2 == SCE_JAVA_TRIPLE_TEMPLATE);

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state <= SCE_JAVA_TASKMARKER;
}

// for java.util.Formatter
// https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/Formatter.html

constexpr bool IsFormatSpecifier(char ch) noexcept {
	return AnyOf(ch, 'a', 'A',
					'b', 'B',
					'c', 'C',
					'd',
					'e', 'E',
					'f',
					'g', 'G',
					'h', 'H',
					'n',
					'o',
					's', 'S',
					'x', 'X');
}

constexpr bool IsDateTimeFormatSpecifier(char ch) noexcept {
	return AnyOf(ch, 'H', 'I', 'k', 'l', 'M', 'S', 'L', 'N', 'p', 'z', 'Z', 's', 'Q', // time
		'B', 'b', 'h', 'A', 'a', 'C', 'Y', 'y', 'j', 'm', 'd', 'e', // date
		'R', 'T', 'r', 'D', 'F', 'c'); // date/time
}

Sci_Position CheckFormatSpecifier(const StyleContext &sc, LexAccessor &styler, bool insideUrl) noexcept {
	if (sc.chNext == '%') {
		return 2;
	}
	if (insideUrl && IsHexDigit(sc.chNext)) {
		// percent encoded URL string
		return 0;
	}
	if (IsASpaceOrTab(sc.chNext) && IsADigit(sc.chPrev)) {
		// ignore word after percent: "5% x"
		return 0;
	}

	Sci_PositionU pos = sc.currentPos + 1;
	// [argument_index$]
	if (sc.chNext == '<') {
		++pos;
	}
	char ch = styler[pos];
	while (IsADigit(ch)) {
		ch = styler[++pos];
	}
	if (ch == '$' && IsADigit(sc.chNext)) {
		ch = styler[++pos];
	}
	// [flags]
	while (AnyOf(ch, ' ', '+', '-', '#', '0', '(', ',')) {
		ch = styler[++pos];
	}
	// [width]
	while (IsADigit(ch)) {
		ch = styler[++pos];
	}
	// [.precision]
	if (ch == '.') {
		ch = styler[++pos];
		while (IsADigit(ch)) {
			ch = styler[++pos];
		}
	}
	// conversion
	if (ch == 't' || ch == 'T') {
		const char chNext = styler[pos + 1];
		if (IsDateTimeFormatSpecifier(chNext)) {
			return pos - sc.currentPos + 2;
		}
	}
	if (IsFormatSpecifier(ch)) {
		return pos - sc.currentPos + 1;
	}
	return 0;
}

inline bool MatchSealed(LexAccessor &styler, Sci_PositionU pos, Sci_PositionU endPos) noexcept {
	char s[8]{};
	styler.GetRange(pos, endPos, s, sizeof(s));
	if (StrStartsWith(s, "ealed")) {
		const uint8_t ch = s[CStrLen("ealed")];
		return ch <= ' ' || ch == '/'; // space or comment
	}
	return false;
}

void ColouriseJavaDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	int lineStateLineType = 0;
	bool insideUrl = false;

	KeywordType kwType = KeywordType::None;
	int chBeforeIdentifier = 0;
	std::vector<int> nestedState;	// string template STR."\{}"

	int visibleChars = 0;
	int chBefore = 0;
	int visibleCharsBefore = 0;
	int chPrevNonWhite = 0;
	DocTagState docTagState = DocTagState::None;
	EscapeSequence escSeq;

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		int lineState = styler.GetLineState(sc.currentLine - 1);
		/*
		2: lineStateLineType
		3: nestedState count
		3*4: nestedState
		*/
		lineState >>= 8;
		if (lineState) {
			UnpackLineState(lineState, nestedState);
		}
	}
	if (startPos == 0) {
		if (sc.Match('#', '!')) {
			// Shell Shebang at beginning of file
			sc.SetState(SCE_JAVA_COMMENTLINE);
			sc.Forward();
			lineStateLineType = JavaLineStateMaskLineComment;
		}
	} else if (IsSpaceEquiv(initStyle)) {
		LookbackNonWhite(styler, startPos, SCE_JAVA_TASKMARKER, chPrevNonWhite, initStyle);
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_JAVA_OPERATOR:
		case SCE_JAVA_OPERATOR2:
			sc.SetState(SCE_JAVA_DEFAULT);
			break;

		case SCE_JAVA_NUMBER:
			if (!IsDecimalNumberEx(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_JAVA_DEFAULT);
			}
			break;

		case SCE_JAVA_IDENTIFIER:
		case SCE_JAVA_ANNOTATION:
			if (!IsIdentifierCharEx(sc.ch)) {
				if (sc.state == SCE_JAVA_ANNOTATION) {
					if (sc.ch == '.' || sc.ch == '$') {
						sc.SetState(SCE_JAVA_OPERATOR);
						sc.ForwardSetState(SCE_JAVA_ANNOTATION);
						continue;
					}
				} else {
					char s[128];
					sc.GetCurrent(s, sizeof(s));
					if (s[0] == '@') {
						if (StrEqual(s, "@interface")) {
							sc.ChangeState(SCE_JAVA_WORD);
							kwType = KeywordType::Annotation;
						} else {
							sc.ChangeState(SCE_JAVA_ANNOTATION);
							continue;
						}
					} else if (keywordLists[KeywordIndex_Keyword].InList(s)) {
						sc.ChangeState(SCE_JAVA_WORD);
						if (StrEqual(s, "import")) {
							if (visibleChars == sc.LengthCurrent()) {
								lineStateLineType = JavaLineStateMaskImport;
							}
						} else if (StrEqualsAny(s, "class", "new", "extends", "instanceof", "throws")) {
							kwType = KeywordType::Class;
						} else if (StrEqualsAny(s, "interface", "implements")) {
							kwType = KeywordType::Interface;
						} else if (StrEqual(s, "enum")) {
							kwType = KeywordType::Enum;
						} else if (StrEqual(s, "record")) {
							kwType = KeywordType::Record;
						} else if (StrEqualsAny(s, "break", "continue")) {
							kwType = KeywordType::Label;
						} else if (StrEqualsAny(s, "return", "yield")) {
							kwType = KeywordType::Return;
						} else if (StrEqualsAny(s, "if", "while")) {
							// to avoid treating following code as type cast:
							// if (identifier) expression, while (identifier) expression
							kwType = KeywordType::While;
						}
						if (kwType > KeywordType::None && kwType < KeywordType::Return) {
							const int chNext = sc.GetDocNextChar();
							if (!IsIdentifierStartEx(chNext)) {
								kwType = KeywordType::None;
							}
						}
					} else if (sc.Match('-', 's') && StrEqual(s, "non") && MatchSealed(styler, sc.currentPos + 2, sc.lineStartNext)) {
						// the non-sealed keyword
						sc.ChangeState(SCE_JAVA_WORD);
						sc.Advance(CStrLen("sealed") + 1);
					} else if (keywordLists[KeywordIndex_Type].InList(s)) {
						sc.ChangeState(SCE_JAVA_WORD2);
					} else if (keywordLists[KeywordIndex_Directive].InList(s)) {
						sc.ChangeState(SCE_JAVA_DIRECTIVE);
					} else if (keywordLists[KeywordIndex_Class].InList(s)) {
						sc.ChangeState(SCE_JAVA_CLASS);
					} else if (keywordLists[KeywordIndex_Interface].InList(s)) {
						sc.ChangeState(SCE_JAVA_INTERFACE);
					} else if (keywordLists[KeywordIndex_Enumeration].InList(s)) {
						sc.ChangeState(SCE_JAVA_ENUM);
					} else if (keywordLists[KeywordIndex_Constant].InList(s)) {
						sc.ChangeState(SCE_JAVA_CONSTANT);
					} else if (sc.ch == ':') {
						if (sc.chNext == ':') {
							// type::method
							sc.ChangeState(SCE_JAVA_CLASS);
						} else if (IsJumpLabelPrevChar(chBefore)) {
							sc.ChangeState(SCE_JAVA_LABEL);
						}
					} else if (sc.ch != '.') {
						if (kwType > KeywordType::None && kwType < KeywordType::Return) {
							sc.ChangeState(static_cast<int>(kwType));
						} else {
							const int chNext = sc.GetDocNextChar(sc.ch == ')');
							if (sc.ch == ')') {
								if (chBeforeIdentifier == '(' && (chNext == '(' || (kwType != KeywordType::While && IsIdentifierCharEx(chNext)))) {
									// (type)(expression)
									// (type)expression, (type)++identifier, (type)--identifier
									sc.ChangeState(SCE_JAVA_CLASS);
								}
							} else if (chNext == '(') {
								// type method()
								// type[] method()
								// type<type> method()
								if (kwType != KeywordType::Return && (IsIdentifierCharEx(chBefore) || chBefore == ']')) {
									sc.ChangeState(SCE_JAVA_FUNCTION_DEFINITION);
								} else {
									sc.ChangeState(SCE_JAVA_FUNCTION);
								}
							} else if (sc.Match('[', ']')
								|| (sc.ch == '<' && (sc.chNext == '>' || sc.chNext == '?'))
								|| (chBeforeIdentifier == '<' && (chNext == '>' || chNext == '<'))
								|| IsIdentifierStartEx(chNext)) {
								// type[] identifier
								// TODO: fix C/C++ style: type identifier[]
								// type<>, type<?>, type<? super T>
								// type<type>
								// type<type<type>>
								// type<type, type>
								// class type implements interface, interface {}
								// type identifier
								sc.ChangeState(SCE_JAVA_CLASS);
							}
						}
					}
					if (sc.state != SCE_JAVA_WORD && sc.ch != '.') {
						kwType = KeywordType::None;
					}
				}
				sc.SetState(SCE_JAVA_DEFAULT);
			}
			break;

		case SCE_JAVA_COMMENTLINE:
			if (sc.atLineStart) {
				sc.SetState(SCE_JAVA_DEFAULT);
			} else {
				HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_JAVA_TASKMARKER);
			}
			break;

		case SCE_JAVA_COMMENTBLOCK:
			if (sc.Match('*', '/')) {
				sc.Forward();
				sc.ForwardSetState(SCE_JAVA_DEFAULT);
			} else if (HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_JAVA_TASKMARKER)) {
				continue;
			}
			break;

		case SCE_JAVA_COMMENTBLOCKDOC:
			switch (docTagState) {
			case DocTagState::At:
				docTagState = DocTagState::None;
				break;
			case DocTagState::InlineAt:
				if (sc.ch == '}') {
					docTagState = DocTagState::None;
					sc.SetState(SCE_JAVA_COMMENTTAGAT);
					sc.ForwardSetState(SCE_JAVA_COMMENTBLOCKDOC);
				}
				break;
			case DocTagState::TagOpen:
			case DocTagState::TagClose:
				if (sc.Match('/', '>') || sc.ch == '>') {
					docTagState = DocTagState::None;
					sc.SetState(SCE_JAVA_COMMENTTAGHTML);
					sc.Forward((sc.ch == '/') ? 2 : 1);
					sc.SetState(SCE_JAVA_COMMENTBLOCKDOC);
				}
				break;
			default:
				break;
			}
			if (sc.Match('*', '/')) {
				sc.Forward();
				sc.ForwardSetState(SCE_JAVA_DEFAULT);
			} else if (sc.ch == '@' && IsAlpha(sc.chNext) && IsCommentTagPrev(sc.chPrev)) {
				docTagState = DocTagState::At;
				sc.SetState(SCE_JAVA_COMMENTTAGAT);
			} else if (sc.Match('{', '@') && IsAlpha(sc.GetRelative(2))) {
				docTagState = DocTagState::InlineAt;
				sc.SetState(SCE_JAVA_COMMENTTAGAT);
				sc.Forward();
			} else if (sc.ch == '<') {
				if (IsAlpha(sc.chNext)) {
					docTagState = DocTagState::TagOpen;
					sc.SetState(SCE_JAVA_COMMENTTAGHTML);
				} else if (sc.chNext == '/' && IsAlpha(sc.GetRelative(2))) {
					docTagState = DocTagState::TagClose;
					sc.SetState(SCE_JAVA_COMMENTTAGHTML);
					sc.Forward();
				}
			} else if (HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_JAVA_TASKMARKER)) {
				continue;
			}
			break;

		case SCE_JAVA_COMMENTTAGAT:
		case SCE_JAVA_COMMENTTAGHTML:
			if (!(IsIdentifierChar(sc.ch) || sc.ch == '-' || sc.ch == ':')) {
				sc.SetState(SCE_JAVA_COMMENTBLOCKDOC);
				continue;
			}
			break;

		case SCE_JAVA_CHARACTER:
		case SCE_JAVA_STRING:
		case SCE_JAVA_TEMPLATE:
		case SCE_JAVA_TRIPLE_TEMPLATE:
		case SCE_JAVA_TRIPLE_STRING:
			if (sc.atLineStart && sc.state <= SCE_JAVA_TEMPLATE) {
				sc.SetState(SCE_JAVA_DEFAULT);
			} else if (sc.ch == '\\') {
				if (sc.chNext == '{' && AnyOf(sc.state, SCE_JAVA_TEMPLATE, SCE_JAVA_TRIPLE_TEMPLATE)) {
					nestedState.push_back(sc.state);
					sc.SetState(SCE_JAVA_OPERATOR2);
					sc.Forward();
				} else if (escSeq.resetEscapeState(sc.state, sc.chNext)) {
					sc.SetState(SCE_JAVA_ESCAPECHAR);
					sc.Forward();
				}
			} else if (sc.ch == '\'' && sc.state == SCE_JAVA_CHARACTER) {
				sc.ForwardSetState(SCE_JAVA_DEFAULT);
			} else if (sc.state != SCE_JAVA_CHARACTER) {
				if (sc.ch == '%') {
					const Sci_Position length = CheckFormatSpecifier(sc, styler, insideUrl);
					if (length != 0) {
						const int state = sc.state;
						sc.SetState(SCE_JAVA_FORMAT_SPECIFIER);
						sc.Advance(length);
						sc.SetState(state);
						continue;
					}
				} else if (sc.ch == '{') {
					if (IsADigit(sc.chNext)) {
						escSeq.outerState = sc.state;
						sc.SetState(SCE_JAVA_PLACEHOLDER);
					}
				} else if (sc.ch == '"' && (sc.state <= SCE_JAVA_TEMPLATE || sc.MatchNext('"', '"'))) {
					if (sc.state > SCE_JAVA_TEMPLATE) {
						sc.Advance(2);
					}
					sc.ForwardSetState(SCE_JAVA_DEFAULT);
				} else if (sc.Match(':', '/', '/') && IsLowerCase(sc.chPrev)) {
					insideUrl = true;
				} else if (insideUrl && IsInvalidUrlChar(sc.ch)) {
					insideUrl = false;
				}
			}
			break;

		case SCE_JAVA_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_JAVA_PLACEHOLDER:
			// for java.text.MessageFormat, only simplest form: {num}
			// https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/text/MessageFormat.html
			if (!IsADigit(sc.ch)) {
				if (sc.ch != '}') {
					sc.Rewind();
					sc.ChangeState(escSeq.outerState);
				}
				sc.Forward();
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;
		}

		if (sc.state == SCE_JAVA_DEFAULT) {
			if (sc.Match('/', '/')) {
				visibleCharsBefore = visibleChars;
				sc.SetState(SCE_JAVA_COMMENTLINE);
				if (visibleChars == 0) {
					lineStateLineType = JavaLineStateMaskLineComment;
				}
			} else if (sc.Match('/', '*')) {
				visibleCharsBefore = visibleChars;
				docTagState = DocTagState::None;
				sc.SetState(SCE_JAVA_COMMENTBLOCK);
				sc.Forward(2);
				if (sc.ch == '*' && sc.chNext != '*') {
					sc.ChangeState(SCE_JAVA_COMMENTBLOCKDOC);
				}
				continue;
			} else if (sc.ch == '\"') {
				insideUrl = false;
				if (sc.MatchNext('"', '"')) {
					sc.SetState((sc.chPrev == '.') ? SCE_JAVA_TRIPLE_TEMPLATE : SCE_JAVA_TRIPLE_STRING);
					sc.Advance(2);
				} else {
					sc.SetState((sc.chPrev == '.') ? SCE_JAVA_TEMPLATE : SCE_JAVA_STRING);
				}
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_JAVA_CHARACTER);
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_JAVA_NUMBER);
			} else if (IsIdentifierStartEx(sc.ch) || sc.Match('@', 'i')) {
				chBefore = chPrevNonWhite;
				if (chPrevNonWhite != '.') {
					chBeforeIdentifier = chPrevNonWhite;
				}
				sc.SetState(SCE_JAVA_IDENTIFIER);
			} else if (sc.ch == '@' && IsIdentifierStartEx(sc.chNext)) {
				sc.SetState(SCE_JAVA_ANNOTATION);
			} else if (IsAGraphic(sc.ch) && sc.ch != '\\') {
				sc.SetState(SCE_JAVA_OPERATOR);
				if (!nestedState.empty()) {
					sc.ChangeState(SCE_JAVA_OPERATOR2);
					if (sc.ch == '{') {
						nestedState.push_back(SCE_JAVA_DEFAULT);
					} else if (sc.ch == '}') {
						const int outerState = TakeAndPop(nestedState);
						sc.ForwardSetState(outerState);
						continue;
					}
				}
			}
		}

		if (!isspacechar(sc.ch)) {
			visibleChars++;
			if (!IsSpaceEquiv(sc.state)) {
				chPrevNonWhite = sc.ch;
			}
		}
		if (sc.atLineEnd) {
			if (!nestedState.empty()) {
				lineStateLineType |= PackLineState(nestedState) << 8;
			}
			styler.SetLineState(sc.currentLine, lineStateLineType);
			lineStateLineType = 0;
			visibleChars = 0;
			visibleCharsBefore = 0;
			docTagState = DocTagState::None;
			kwType = KeywordType::None;
		}
		sc.Forward();
	}

	sc.Complete();
}

struct FoldLineState {
	int lineComment;
	int packageImport;
	constexpr explicit FoldLineState(int lineState) noexcept:
		lineComment(lineState & JavaLineStateMaskLineComment),
		packageImport((lineState >> 1) & 1) {
	}
};

void FoldJavaDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList /*keywordLists*/, Accessor &styler) {
	const Sci_PositionU endPos = startPos + lengthDoc;
	Sci_Line lineCurrent = styler.GetLine(startPos);
	FoldLineState foldPrev(0);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
		foldPrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
		const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent - 1, SCE_JAVA_OPERATOR, SCE_JAVA_TASKMARKER);
		if (bracePos) {
			startPos = bracePos + 1; // skip the brace
		}
	}

	int levelNext = levelCurrent;
	FoldLineState foldCurrent(styler.GetLineState(lineCurrent));
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = sci::min(lineStartNext, endPos);

	int styleNext = styler.StyleAt(startPos);
	int style = initStyle;
	int visibleChars = 0;

	while (startPos < endPos) {
		const int stylePrev = style;
		style = styleNext;
		styleNext = styler.StyleAt(++startPos);

		switch (style) {
		case SCE_JAVA_COMMENTBLOCK:
		case SCE_JAVA_COMMENTBLOCKDOC:
		case SCE_JAVA_TRIPLE_STRING:
		case SCE_JAVA_TRIPLE_TEMPLATE:
			if (style != stylePrev) {
				levelNext++;
			}
			if (style != styleNext) {
				levelNext--;
			}
			break;

		case SCE_JAVA_OPERATOR:
		case SCE_JAVA_OPERATOR2: {
			const char ch = styler[startPos - 1];
			if (ch == '{' || ch == '[' || ch == '(') {
				levelNext++;
			} else if (ch == '}' || ch == ']' || ch == ')') {
				levelNext--;
			}
		} break;
		}

		if (visibleChars == 0 && !IsSpaceEquiv(style)) {
			++visibleChars;
		}
		if (startPos == lineStartNext) {
			const FoldLineState foldNext(styler.GetLineState(lineCurrent + 1));
			levelNext = sci::max(levelNext, SC_FOLDLEVELBASE);
			if (foldCurrent.lineComment) {
				levelNext += foldNext.lineComment - foldPrev.lineComment;
			} else if (foldCurrent.packageImport) {
				levelNext += foldNext.packageImport - foldPrev.packageImport;
			} else if (visibleChars) {
				const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent, SCE_JAVA_OPERATOR, SCE_JAVA_TASKMARKER);
				if (bracePos) {
					levelNext++;
					startPos = bracePos + 1; // skip the brace
					style = SCE_JAVA_OPERATOR;
					styleNext = styler.StyleAt(startPos);
				}
			}

			const int levelUse = levelCurrent;
			int lev = levelUse | levelNext << 16;
			if (levelUse < levelNext) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			styler.SetLevel(lineCurrent, lev);

			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = sci::min(lineStartNext, endPos);
			levelCurrent = levelNext;
			foldPrev = foldCurrent;
			foldCurrent = foldNext;
			visibleChars = 0;
		}
	}
}

}

LexerModule lmJava(SCLEX_JAVA, ColouriseJavaDoc, "java", FoldJavaDoc);
