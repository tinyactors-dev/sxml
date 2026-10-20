<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>

  <xsl:template match="/library">
    <ul>
      <xsl:for-each select="//book">
        <xsl:variable name="page" select="'books/' || @id || '.html'"/>
        <!-- One page for each book... -->
        <xsl:result-document href="{$page}">
          <h1>{title}</h1>
          <p>{author => string-join(' and ')}, {@year}</p>
        </xsl:result-document>
        <!-- ...and a link to it in the index. -->
        <li><a href="{$page}">{title}</a></li>
      </xsl:for-each>
    </ul>
  </xsl:template>
</xsl:stylesheet>
