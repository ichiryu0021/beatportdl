package beatport

import (
	"encoding/json"
	"fmt"
	"net/url"
)

// BuildLatestParams 构造 /catalog/tracks/? 和 /catalog/releases/? 共用的查询参数串。
// 导出以便测试覆盖（test/beatport）。
func BuildLatestParams(genreName string, page int, perPage int, dateFrom string, dateTo string) string {
	params := fmt.Sprintf("page=%d&per_page=%d&is_available_for_streaming=true", page, perPage)
	if genreName != "" {
		params += "&genre_name=" + url.QueryEscape(genreName)
	}
	if dateFrom != "" || dateTo != "" {
		params += "&new_release_date=" + dateFrom + ":" + dateTo
	}
	return params
}

func (b *Beatport) GetLatestTracks(genreName string, page int, perPage int, dateFrom string, dateTo string) (*Paginated[Track], error) {
	res, err := b.fetch(
		"GET",
		fmt.Sprintf("/catalog/tracks/?order_by=-publish_date&%s", BuildLatestParams(genreName, page, perPage, dateFrom, dateTo)),
		nil,
		"",
	)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var response Paginated[Track]
	if err = json.NewDecoder(res.Body).Decode(&response); err != nil {
		return nil, err
	}
	return &response, nil
}

func (b *Beatport) GetLatestReleases(genreName string, page int, perPage int, dateFrom string, dateTo string) (*Paginated[Release], error) {
	res, err := b.fetch(
		"GET",
		fmt.Sprintf("/catalog/releases/?order_by=-new_release_date&%s", BuildLatestParams(genreName, page, perPage, dateFrom, dateTo)),
		nil,
		"",
	)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var response Paginated[Release]
	if err = json.NewDecoder(res.Body).Decode(&response); err != nil {
		return nil, err
	}
	return &response, nil
}

func (b *Beatport) GetGenres() (*Paginated[Genre], error) {
	res, err := b.fetch(
		"GET",
		"/catalog/genres/",
		nil,
		"",
	)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	var response Paginated[Genre]
	if err = json.NewDecoder(res.Body).Decode(&response); err != nil {
		return nil, err
	}
	return &response, nil
}
