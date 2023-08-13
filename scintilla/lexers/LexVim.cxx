// This file is part of Notepad2.
// See License.txt for details about distribution and modification.
//! Lexer for Vim.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>

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

using namespace Lexilla;

namespace {

constexpr bool IsVimEscapeChar(int ch) noexcept {
	return AnyOf(ch, '\\', '\"', 'b', 'e', 'f', 'n', 'r', 't');
}

struct EscapeSequence {
	int digitsLeft = 0;
	bool hex = false;

	bool resetEscapeState(int chNext) noexcept {
		// https://vimhelp.org/eval.txt.html#string
		digitsLeft = 0;
		hex = true;
		if (chNext == 'x' || chNext == 'X') {
			digitsLeft = 3;
		} else if (chNext == 'u') {
			digitsLeft = 5;
		} else if (chNext == 'U') {
			digitsLeft = 9;
		} else if (IsOctalDigit(chNext)) {
			digitsLeft = 3;
			hex = false;
		} else if (IsVimEscapeChar(chNext)) {
			digitsLeft = 1;
		}
		return digitsLeft != 0;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsOctalOrHex(ch, hex);
	}
};

enum {
	VimLineStateMaskLineComment = 1 << 0,		// line comment
	VimLineStateMaskLineContinuation = 1 << 1,	// line continuation
	VimLineStateMaskAutoCommand = 1 << 2,		// autocmd
	VimLineStateMaskVim9Script = 1 << 3,		// vim9script
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Command = 1,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

enum class KeywordType {
	None,
	Export,		// export def
};

void ColouriseVimDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	int lineState = 0;
	int lineVisibleChars = 0;
	int logicalVisibleChars = 0;
	KeywordType kwType = KeywordType::None;
	bool preferRegex = false;
	bool insideRegexRange = false; // inside regex character range []
	EscapeSequence escSeq;

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		lineState = styler.GetLineState(sc.currentLine - 1);
		lineState &= VimLineStateMaskAutoCommand | VimLineStateMaskVim9Script;
	} else if (startPos == 0 && sc.Match('#', '!')) {
		// Shell Shebang at beginning of file
		sc.SetState(SCE_VIM_COMMENTLINE);
		sc.Forward();
		lineState = VimLineStateMaskLineComment;
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_VIM_OPERATOR:
			sc.SetState(SCE_VIM_DEFAULT);
			break;

		case SCE_VIM_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_IDENTIFIER:
			if (!IsIdentifierChar(sc.ch)) {
				char s[128];
				const KeywordType kwPrev = kwType;
				kwType = KeywordType::None;
				sc.GetCurrent(s, sizeof(s));
				if (keywordLists[KeywordIndex_Keyword].InList(s)) {
					if (!(lineState & VimLineStateMaskAutoCommand) && logicalVisibleChars == sc.LengthCurrent()) {
						sc.ChangeState(SCE_VIM_WORD);
						if (StrEqualsAny(s, "au", "autocmd")) {
							lineState |= VimLineStateMaskAutoCommand;
						} else if (StrEqual(s, "export")) {
							kwType = KeywordType::Export;
						}
					} else {
						if (kwPrev == KeywordType::Export && StrEqual(s, "def")) {
							sc.ChangeState(SCE_VIM_WORD);
						} else {
							sc.ChangeState(SCE_VIM_WORD_DEMOTED);
						}
					}
				} else if (keywordLists[KeywordIndex_Command].InList(s)) {
					sc.ChangeState(SCE_VIM_COMMANDS);
					if (lineVisibleChars == sc.LengthCurrent()) {
						if (StrEqualsAny(s, "syn", "syntax")) {
							const int chNext = sc.GetLineNextChar();
							// syntax match, syntax region
							// https://vimhelp.org/syntax.txt.html#%3Asyn-define
							preferRegex = chNext == 'm' || chNext == 'r';
						} else if (StrEqual(s, "vim9script")) {
							lineState |= VimLineStateMaskVim9Script;
						}
					}
				} else if (sc.GetLineNextChar() == '(') {
					sc.ChangeState(SCE_VIM_FUNCTION);
				}
				sc.SetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_STRING_DQ:
			if (sc.atLineStart) {
				sc.SetState(SCE_VIM_DEFAULT);
			} else if (sc.ch == '\\') {
				if (escSeq.resetEscapeState(sc.chNext)) {
					sc.SetState(SCE_VIM_ESCAPECHAR);
				}
				sc.Forward();
			} else if (sc.ch == '\"') {
				sc.ForwardSetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(SCE_VIM_STRING_DQ);
				continue;
			}
			break;

		case SCE_VIM_STRING_SQ:
			if (sc.atLineStart) {
				sc.SetState(SCE_VIM_DEFAULT);
			} else if (sc.ch == '\'') {
				if (sc.chNext == '\'') {
					sc.SetState(SCE_VIM_ESCAPECHAR);
					sc.Forward();
					sc.ForwardSetState(SCE_VIM_STRING_SQ);
					continue;
				}
				sc.ForwardSetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_REGEX:
			if (sc.atLineStart) {
				sc.SetState(SCE_VIM_DEFAULT);
			} else if (sc.ch == '\\') {
				sc.Forward();
			} else if (sc.ch == '[' || sc.ch == ']') {
				insideRegexRange = sc.ch == '[';
			} else if (sc.ch == '/' && !insideRegexRange) {
				sc.ForwardSetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_COMMENTLINE:
			if (sc.atLineStart) {
				sc.SetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_BLOB_HEX:
			if (!(IsIdentifierChar(sc.ch) || sc.ch == '.')) {
				sc.SetState(SCE_VIM_DEFAULT);
			}
			break;

		case SCE_VIM_ENV_VARIABLE:
		case SCE_VIM_OPTION:
		case SCE_VIM_REGISTER:
			if (!IsIdentifierChar(sc.ch)) {
				sc.SetState(SCE_VIM_DEFAULT);
			}
			break;
		}

		if (sc.state == SCE_VIM_DEFAULT) {
			if (sc.ch == '\"') {
				const int state = (logicalVisibleChars != 0 || (lineState & VimLineStateMaskVim9Script)) ? SCE_VIM_STRING_DQ : SCE_VIM_COMMENTLINE;
				sc.SetState(state);
				if (lineVisibleChars == 0 && state == SCE_VIM_COMMENTLINE) {
					lineState |= VimLineStateMaskLineComment;
				}
			} else if (sc.ch == '#') { // vim 9
				sc.SetState((sc.chPrev <= ' ') ? SCE_VIM_COMMENTLINE : SCE_VIM_OPERATOR);
				if (lineVisibleChars == 0) {
					lineState |= VimLineStateMaskLineComment;
				}
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_VIM_STRING_SQ);
			} else if (sc.ch == '0' && UnsafeLower(sc.chNext) == 'z') {
				sc.SetState(SCE_VIM_BLOB_HEX);
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_VIM_NUMBER);
			} else if ((sc.ch == '$' || sc.ch == '&') && IsIdentifierChar(sc.chNext)) {
				sc.SetState((sc.ch == '$') ? SCE_VIM_ENV_VARIABLE : SCE_VIM_OPTION);
				sc.Forward();
			} else if (sc.ch == '@') {
				sc.SetState(SCE_VIM_REGISTER);
				sc.Forward();
			} else if (sc.ch == '\\' && logicalVisibleChars != 0) {
				sc.Forward();
			} else if (IsIdentifierStart(sc.ch)) {
				if (sc.chNext == ':' && IsLowerCase(sc.ch)) {
					sc.SetState(SCE_VIM_ENV_VARIABLE); // internal variable namespace
					sc.ForwardSetState(SCE_VIM_OPERATOR);
				} else {
					sc.SetState(SCE_VIM_IDENTIFIER);
				}
			} else if (sc.ch == '/' && preferRegex && !IsEOLChar(sc.chNext)) {
				insideRegexRange = false;
				sc.SetState(SCE_VIM_REGEX);
			} else if (IsAGraphic(sc.ch)) {
				sc.SetState(SCE_VIM_OPERATOR);
				if (sc.ch == '|' && sc.chNext != '|' && !(lineState & VimLineStateMaskAutoCommand)) {
					// pipe, change logicalVisibleChars to 0 in next block
					logicalVisibleChars = -1;
				}
			}
		}

		if (!isspacechar(sc.ch) && !(lineVisibleChars == 0 && sc.ch == ':')) {
			if (lineVisibleChars == 0) {
				if (sc.ch == '\\') {
					lineState |= VimLineStateMaskLineContinuation;
				} else {
					lineState &= ~VimLineStateMaskAutoCommand;
				}
			}
			lineVisibleChars++;
			logicalVisibleChars++;
		}

		if (sc.atLineEnd) {
			styler.SetLineState(sc.currentLine, lineState);
			lineState &= VimLineStateMaskAutoCommand | VimLineStateMaskVim9Script;
			lineVisibleChars = 0;
			logicalVisibleChars = 0;
			preferRegex = false;
		}
		sc.Forward();
	}

	sc.Complete();
}

struct FoldLineState {
	int lineComment;
	int lineContinuation;
	constexpr explicit FoldLineState(int lineState) noexcept:
		lineComment(lineState & VimLineStateMaskLineComment),
		lineContinuation((lineState >> 1) & 1) {
	}
};

void FoldVimDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int /*initStyle*/, LexerWordList, Accessor &styler) {
	const Sci_PositionU endPos = startPos + lengthDoc;
	Sci_Line lineCurrent = styler.GetLine(startPos);
	FoldLineState foldPrev(0);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
		foldPrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
	}

	int levelNext = levelCurrent;
	FoldLineState foldCurrent(styler.GetLineState(lineCurrent));
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = sci::min(lineStartNext, endPos);

	char buf[8]; // while
	constexpr int MaxFoldWordLength = sizeof(buf) - 1;
	int wordLen = 0;

	int styleNext = styler.StyleAt(startPos);
	while (startPos < endPos) {
		const int style = styleNext;
		styleNext = styler.StyleAt(++startPos);

		if (style == SCE_VIM_WORD) {
			if (wordLen < MaxFoldWordLength) {
				buf[wordLen++] = styler[startPos - 1];
			}
			if (styleNext != SCE_VIM_WORD) {
				buf[wordLen] = '\0';
				wordLen = 0;
				if (StrEqualsAny(buf, "if", "while", "for", "try", "def") || StrStartsWith(buf, "fun")) {
					levelNext++;
				} else if (StrStartsWith(buf, "end")) {
					levelNext--;
				}
			}
		}

		if (startPos == lineStartNext) {
			const FoldLineState foldNext(styler.GetLineState(lineCurrent + 1));
			if (foldCurrent.lineComment) {
				levelNext += foldNext.lineComment - foldPrev.lineComment;
			}
			levelNext += foldNext.lineContinuation - foldCurrent.lineContinuation;

			const int levelUse = levelCurrent;
			int lev = levelUse | levelNext << 16;
			if (levelUse < levelNext) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			if (lev != styler.LevelAt(lineCurrent)) {
				styler.SetLevel(lineCurrent, lev);
			}

			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = sci::min(lineStartNext, endPos);
			levelCurrent = levelNext;
			foldPrev = foldCurrent;
			foldCurrent = foldNext;
		}
	}
}

}

LexerModule lmVim(SCLEX_VIM, ColouriseVimDoc, "vim", FoldVimDoc);
