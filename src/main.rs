mod parser;

use parser::lexer::Lexer;
use parser::parser::Parser;

fn main() {
    let queries = [
        "SELECT * FROM orders",
        "SELECT c.name, c.email FROM customers c WHERE c.country = 'US'",
        "SELECT o.id, SUM(o.total) FROM orders o \
         INNER JOIN customers c ON o.customer_id = c.id \
         WHERE c.active = TRUE \
         GROUP BY o.id \
         HAVING SUM(o.total) > 100 \
         ORDER BY o.id DESC \
         LIMIT 20",
        // precedence: OR(a=1, AND(b=2, c=3))
        "SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3",
    ];

    for sql in &queries {
        println!("SQL: {sql}");
        let tokens = Lexer::new(sql).tokenize().unwrap();
        match Parser::new(tokens).parse() {
            Ok(ast) => println!("AST: {ast:#?}\n"),
            Err(e)  => println!("Error: {e}\n"),
        }
    }
}
