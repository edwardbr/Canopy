use proc_macro::{Delimiter, Group, Ident, Punct, Spacing, Span, TokenStream, TokenTree};

#[proc_macro]
pub fn paste(input: TokenStream) -> TokenStream {
    expand_stream(input)
}

fn expand_stream(input: TokenStream) -> TokenStream {
    let mut output = TokenStream::new();

    for token in input {
        match token {
            TokenTree::Group(group) => {
                if let Some(ident) = try_expand_paste_ident(&group) {
                    output.extend([TokenTree::Ident(ident)]);
                } else {
                    let mut rebuilt = Group::new(group.delimiter(), expand_stream(group.stream()));
                    rebuilt.set_span(group.span());
                    output.extend([TokenTree::Group(rebuilt)]);
                }
            }
            other => output.extend([other]),
        }
    }

    output
}

fn try_expand_paste_ident(group: &Group) -> Option<Ident> {
    if group.delimiter() != Delimiter::Bracket {
        return None;
    }

    let tokens: Vec<TokenTree> = group.stream().into_iter().collect();
    if tokens.len() < 3 {
        return None;
    }

    let first = match &tokens[0] {
        TokenTree::Punct(punct) if punct.as_char() == '<' => punct,
        _ => return None,
    };

    match tokens.last() {
        Some(TokenTree::Punct(punct)) if punct.as_char() == '>' => {}
        _ => return None,
    }

    let mut name = String::new();
    for token in &tokens[1..tokens.len() - 1] {
        match token {
            TokenTree::Ident(ident) => name.push_str(&ident.to_string()),
            TokenTree::Punct(punct) => {
                let ch = punct.as_char();
                if ch == '_' || ch.is_ascii_alphanumeric() {
                    name.push(ch);
                }
            }
            TokenTree::Literal(literal) => name.push_str(&literal.to_string().replace('"', "")),
            TokenTree::Group(inner) => {
                name.push_str(&expand_stream(inner.stream()).to_string().replace(' ', ""))
            }
        }
    }

    if name.is_empty() {
        return None;
    }

    Some(Ident::new(&name, span_from_punct(first)))
}

fn span_from_punct(punct: &Punct) -> Span {
    let mut span = punct.span();
    if punct.spacing() == Spacing::Joint {
        span = punct.span();
    }
    span
}
