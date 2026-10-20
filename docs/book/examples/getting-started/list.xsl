<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>
  <xsl:template match="/books">
    <html>
      <body>
        <h1>Books</h1>
        <ul>
          <xsl:apply-templates select="book">
            <xsl:sort select="title"/>
          </xsl:apply-templates>
        </ul>
      </body>
    </html>
  </xsl:template>
  <xsl:template match="book">
    <li>{title} ({@year})</li>
  </xsl:template>
</xsl:stylesheet>
