ACTION query_seed(users, minimum_score)
    active = QUERY user IN users
        WHERE user.enabled
        WHERE user.score >= minimum_score
        ORDER BY user.score DESCENDING, user.id ASCENDING
        TAKE 12
        SELECT user.id
    END QUERY
END ACTION