#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    // Keywords
    Select,
    From,
    Where,
    Join,
    Inner,
    Left,
    Right,
    Cross,
    On,
    And,
    Or,
    Not,
    As,
    Order,
    By,
    Group,
    Limit,
    Asc,
    Desc,
    Having,
    Null,
    True,
    False,
    // Values
    Identifier(String),
    Integer(i64),
    Float(f64),
    StringLit(String),

    // Operators
    Eq,
    Neq,
    Lt,
    Gt,
    Lte,
    Gte,
    Plus,
    Minus,
    Star,
    Slash,

    // Punctuation
    Comma,
    Dot,
    LParen,
    RParen,

    Eof,
}

pub struct Lexer {
    input: Vec<char>,
    pos: usize, //current position in the string
}

impl Lexer {
    pub fn new(input: &str) -> Self {
        Lexer {
            input: input.chars().collect(),
            pos: 0,
        }
    }

    fn peek(&self) -> Option<char> {
        self.input.get(self.pos).copied()
    }

    fn advance(&mut self) -> Option<char> {
        let ch = self.input.get(self.pos).copied();
        self.pos += 1;
        ch
    }

    fn skip_whitespace(&mut self) {
        while let Some(ch) = self.peek() {
            if ch.is_whitespace() {
                self.advance();
            } else {
                break;
            }
        }
    }

    pub fn tokenize(&mut self) -> Result<Vec<Token>, String> {
        let mut tokens = Vec::new();
        loop {
            let tok = self.next_token()?;
            if tok == Token::Eof {
                tokens.push(tok);
                break;
            }
            tokens.push(tok);
        }
        Ok(tokens)
    }

    pub fn next_token(&mut self) -> Result<Token, String> {
        self.skip_whitespace();

        match self.peek() {
            None => Ok(Token::Eof),

            Some(',') => {
                self.advance();
                Ok(Token::Comma)
            }
            Some('.') => {
                self.advance();
                Ok(Token::Dot)
            }
            Some('(') => {
                self.advance();
                Ok(Token::LParen)
            }
            Some(')') => {
                self.advance();
                Ok(Token::RParen)
            }
            Some('+') => {
                self.advance();
                Ok(Token::Plus)
            }
            Some('-') => {
                self.advance();
                Ok(Token::Minus)
            }
            Some('*') => {
                self.advance();
                Ok(Token::Star)
            }
            Some('/') => {
                self.advance();
                Ok(Token::Slash)
            }
            Some('=') => {
                self.advance();
                Ok(Token::Eq)
            }

            Some('<') => {
                self.advance();
                match self.peek() {
                    Some('=') => {
                        self.advance();
                        Ok(Token::Lte)
                    }
                    Some('>') => {
                        self.advance();
                        Ok(Token::Neq)
                    }
                    _ => Ok(Token::Lt),
                }
            }
            Some('>') => {
                self.advance();
                match self.peek() {
                    Some('=') => {
                        self.advance();
                        Ok(Token::Gte)
                    }
                    _ => Ok(Token::Gt),
                }
            }
            Some('!') => {
                self.advance();
                match self.peek() {
                    Some('=') => {
                        self.advance();
                        Ok(Token::Neq)
                    }
                    _ => Err(format!("expected '=' after '!'")),
                }
            }

            Some('\'') => {
                self.advance(); // skip opening quote
                let mut s = String::new();
                loop {
                    match self.peek() {
                        None => return Err("unterminated string literal".into()),
                        Some('\'') => {
                            self.advance(); // consume closing quote
                            break;
                        }
                        Some(ch) => {
                            self.advance();
                            s.push(ch);
                        }
                    }
                }
                Ok(Token::StringLit(s))
            }

            Some(ch) if ch.is_ascii_digit() => {
                let mut num = String::new();
                while let Some(c) = self.peek() {
                    if c.is_ascii_digit() {
                        self.advance();
                        num.push(c);
                    } else {
                        break;
                    }
                }
                if self.peek() == Some('.') {
                    let next = self.input.get(self.pos + 1).copied();
                    if next.map(|c| c.is_ascii_digit()).unwrap_or(false) {
                        self.advance(); // consume '.'
                        num.push('.');
                        while let Some(c) = self.peek() {
                            if c.is_ascii_digit() {
                                self.advance();
                                num.push(c);
                            } else {
                                break;
                            }
                        }
                        let val: f64 = num.parse().map_err(|_| "invalid float")?;
                        return Ok(Token::Float(val));
                    }
                }
                let val: i64 = num.parse().map_err(|_| "invalid integer")?;
                Ok(Token::Integer(val))
            }

            Some(ch) if ch.is_alphabetic() || ch == '_' => {
                let mut word = String::new();
                while let Some(c) = self.peek() {
                    if c.is_alphanumeric() || c == '_' {
                        self.advance();
                        word.push(c);
                    } else {
                        break;
                    }
                }
                match word.to_uppercase().as_str() {
                    "SELECT" => Ok(Token::Select),
                    "FROM" => Ok(Token::From),
                    "WHERE" => Ok(Token::Where),
                    "JOIN" => Ok(Token::Join),
                    "INNER" => Ok(Token::Inner),
                    "LEFT" => Ok(Token::Left),
                    "RIGHT" => Ok(Token::Right),
                    "CROSS" => Ok(Token::Cross),
                    "ON" => Ok(Token::On),
                    "AND" => Ok(Token::And),
                    "OR" => Ok(Token::Or),
                    "NOT" => Ok(Token::Not),
                    "AS" => Ok(Token::As),
                    "ORDER" => Ok(Token::Order),
                    "BY" => Ok(Token::By),
                    "GROUP" => Ok(Token::Group),
                    "LIMIT" => Ok(Token::Limit),
                    "ASC" => Ok(Token::Asc),
                    "DESC" => Ok(Token::Desc),
                    "HAVING" => Ok(Token::Having),
                    "NULL" => Ok(Token::Null),
                    "TRUE" => Ok(Token::True),
                    "FALSE" => Ok(Token::False),
                    _ => Ok(Token::Identifier(word)),
                }
            }

            Some(ch) => Err(format!("unexpected character: '{}'", ch)),
        }
    }
}
