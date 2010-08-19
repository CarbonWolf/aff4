// $ANTLR : "aff4il.g" -> "AFF4ILAntlrParser.java"$

package aff4.infomodel.aff4il.parser ;
import aff4.infomodel.aff4il.parser.AntlrUtils ;
import aff4.infomodel.aff4il.parser.AFF4ILParserEventHandler ;
import java.io.* ;
import antlr.TokenStreamRecognitionException ;

public interface AFF4ILAntlrParserTokenTypes {
	int EOF = 1;
	int NULL_TREE_LOOKAHEAD = 3;
	int ANON = 4;
	int FORMULA = 5;
	int QNAME = 6;
	int SEP = 7;
	int KEYWORD = 8;
	int NAME_OP = 9;
	int KW_THIS = 10;
	int KW_OF = 11;
	int KW_HAS = 12;
	int KW_A = 13;
	int KW_IS = 14;
	int AT_PREFIX = 15;
	int AT_LANG = 16;
	int STRING = 17;
	int LITERAL = 18;
	int LCURLY = 19;
	int RCURLY = 20;
	int SEMI = 21;
	int COMMA = 22;
	int PATH = 23;
	int RPATH = 24;
	int EQUAL = 25;
	int ARROW_R = 26;
	int ARROW_L = 27;
	int ARROW_PATH_L = 28;
	int ARROW_PATH_R = 29;
	int NUMBER = 30;
	int DATATYPE = 31;
	int URIREF = 32;
	int UVAR = 33;
	int THING = 34;
	int URI_OR_IMPLIES = 35;
	int URICHAR = 36;
	int AT_WORD = 37;
	int XNAMECHAR = 38;
	int EXNAMECHAR = 39;
	int XNAME = 40;
	int NSNAME = 41;
	int LNAME = 42;
	int SEP_OR_PATH = 43;
	int DOT = 44;
	int AT = 45;
	int LPAREN = 46;
	int RPAREN = 47;
	int LBRACK = 48;
	int RBRACK = 49;
	int LANGLE = 50;
	int RANGLE = 51;
	int NAME_IT = 52;
	int QUESTION = 53;
	int ARROW_MEANS = 54;
	int COLON = 55;
	int SL_COMMENT = 56;
	int NL1 = 57;
	int NL2 = 58;
	int NL3 = 59;
	int NL = 60;
	int WS = 61;
	int NWS = 62;
	int ALPHA = 63;
	int NUMERIC = 64;
	int ALPHANUMERIC = 65;
	int NON_ANC = 66;
	int STRING1 = 67;
	int STRING2 = 68;
	int QUOTE3S = 69;
	int QUOTE3D = 70;
	int ESCAPE = 71;
	int ESC_CHAR = 72;
	int HEX_DIGIT = 73;
	int HEX4 = 74;
}
