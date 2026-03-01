mod parser;

fn main() {
    let sql = "SELECT c.name FROM customers c WHERE c.country = 'US'";
    let mut lexer = parser::lexer::Lexer::new(sql);
    let tokens = lexer.tokenize().unwrap();
    for tok in &tokens {
        println!("{:?}", tok);
    }
}
