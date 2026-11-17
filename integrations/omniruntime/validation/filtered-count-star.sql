SELECT 'single' AS case_name,
       count(*) AS total_rows,
       count(*) FILTER (WHERE cc_employees % 3 = 0) AS selected_rows
FROM call_center;

SELECT cc_state,
       count(*) AS total_rows,
       count(*) FILTER (WHERE cc_employees % 3 = 0) AS selected_rows
FROM call_center
GROUP BY cc_state
ORDER BY cc_state;
